# fleet.ps1 - create, drive and destroy the datagen fleet.
#
#   .\tools\cloud\fleet.ps1 types           what SERVER_TYPE values actually exist
#   .\tools\cloud\fleet.ps1 push-corpus     create the hub dirs, upload the corpus
#   .\tools\cloud\fleet.ps1 push-net        publish the net EVAL=nnue pins by hash
#   .\tools\cloud\fleet.ps1 prepare-corpus  one box, split the corpus, destroy it
#   .\tools\cloud\fleet.ps1 calibrate   one box, measure records/sec, destroy it
#   .\tools\cloud\fleet.ps1 up          create BOXES boxes, provision, launch
#   .\tools\cloud\fleet.ps1 status      server states and per-unit progress
#   .\tools\cloud\fleet.ps1 logs 2      tail box 2's run log
#   .\tools\cloud\fleet.ps1 down        destroy the boxes (the hub keeps the data)
#
# Boxes are disposable and carry no state worth keeping: every finished unit is
# on the hub before the box is allowed to forget it. `down` the moment a run
# ends - Hetzner bills by the hour.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('up', 'down', 'status', 'logs', 'calibrate',
                 'push-corpus', 'push-net', 'prepare-corpus', 'types')]
    [string]$Command = 'status',

    [Parameter(Position = 1)]
    [int]$Box = 0,

    [string]$ConfigPath,

    # push-corpus: the compressed corpus to upload.
    # push-net: the .nnue to upload, if not the exporter's default.
    [string]$Path,

    # prepare-corpus / calibrate: leave the throwaway box running afterwards.
    [switch]$Keep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\common.ps1"

if (-not $ConfigPath) { $ConfigPath = Join-Path $PSScriptRoot 'job.env' }

# job.env is bare KEY=value by contract (see the header there), which is what
# lets both sh and PowerShell read the same file without a parser each.
function Get-JobConfig([string]$Path) {
    if (-not (Test-Path $Path)) {
        throw "no config at $Path. Copy job.env.template to job.env and edit it: " +
              "Copy-Item tools\cloud\job.env.template tools\cloud\job.env"
    }
    $cfg = @{}
    foreach ($line in Get-Content $Path) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        $i = $t.IndexOf('=')
        if ($i -lt 1) { throw "not a KEY=value line in ${Path}: $line" }
        $cfg[$t.Substring(0, $i)] = $t.Substring($i + 1)
    }
    # Caught here rather than as a baffling ssh or hcloud failure ten minutes in.
    $unset = @($cfg.Keys | Where-Object { $cfg[$_] -match 'REPLACE_ME' } | Sort-Object)
    if ($unset.Count -gt 0) {
        throw "still unedited in ${Path}: $($unset -join ', ')"
    }
    return $cfg
}

function Get-Hcloud {
    $h = Find-First @('hcloud', "$env:LOCALAPPDATA\Microsoft\WinGet\Links\hcloud.exe")
    if (-not $h) {
        throw "hcloud CLI not found. winget install HetznerCloud.CLI, then set HCLOUD_TOKEN."
    }
    if (-not $env:HCLOUD_TOKEN) {
        throw "HCLOUD_TOKEN is not set. Create a read/write API token in the Hetzner Cloud console."
    }
    return $h
}

$cfg = Get-JobConfig $ConfigPath
# StrictMode does not catch a missing hashtable key, so a typo'd config would
# reach hcloud as an empty --type and fail somewhere far less obvious.
foreach ($k in @('GEN', 'BOXES', 'HUB', 'HUB_PORT', 'HUB_DIR',
                 'SERVER_TYPE', 'LOCATION', 'IMAGE', 'SSH_KEY')) {
    if (-not $cfg.ContainsKey($k) -or -not $cfg[$k]) { throw "$k is not set in $ConfigPath" }
}

$Gen     = $cfg['GEN']
$Boxes   = [int]$cfg['BOXES']
$HubUser = $cfg['HUB']
$HubPort = $cfg['HUB_PORT']
$HubDir  = $cfg['HUB_DIR']

# Splatted into every ssh and scp call. IdentitiesOnly stops ssh from also
# offering whatever else is in the agent, which on a fresh box just burns
# attempts against MaxAuthTries before the right key is tried.
$KeyFile = ''
$IdentityArgs = @()
if ($cfg.ContainsKey('SSH_KEY_FILE') -and $cfg['SSH_KEY_FILE']) {
    $KeyFile = [Environment]::ExpandEnvironmentVariables($cfg['SSH_KEY_FILE'])
    if (-not (Test-Path $KeyFile)) { throw "SSH_KEY_FILE does not exist: $KeyFile" }
    $IdentityArgs = @('-i', $KeyFile, '-o', 'IdentitiesOnly=yes')
}

# Hetzner recycles IPs aggressively, and this workflow creates and destroys
# boxes all day: a fresh box routinely lands on an address a box from an hour
# ago used, with different host keys. Against the real known_hosts that is a
# hard "REMOTE HOST IDENTIFICATION HAS CHANGED" on every retry until Wait-ForSsh
# gives up - and there is no way to verify a key out of band for a machine that
# gets deleted this afternoon anyway. Boxes get a throwaway file; the hub is
# long-lived and holds the data, so it keeps being checked properly.
$BoxKnownHosts = (Join-Path ([System.IO.Path]::GetTempPath()) "fleet-$Gen.known_hosts") -replace '\\', '/'
# Emptied every invocation: with StrictHostKeyChecking=no this file was never a
# security control, and keeping it only meant a recycled IP within one
# generation still printed the changed-key warning the throwaway file exists to
# avoid.
Remove-Item -LiteralPath $BoxKnownHosts -ErrorAction SilentlyContinue
# Every box-bound ssh and scp goes through this. What each option is for:
#   BatchMode        never prompt. A password prompt against a box that has no
#                    password is an indefinite hang, not an error, and it is
#                    invisible: the command just sits there.
#   ConnectTimeout   a box that is not listening fails in 15s rather than never.
#   ServerAlive*     a connection that dies mid-transfer, or a box that wedges,
#                    ends the session after ~2 minutes of silence. Without this
#                    a dropped TCP connection hangs until someone notices, which
#                    is what made provisioning look random: it was not the build
#                    stalling, it was the transport.
$BoxSsh = $IdentityArgs + @(
    '-o', 'StrictHostKeyChecking=no',
    '-o', "UserKnownHostsFile=$BoxKnownHosts",
    '-o', 'BatchMode=yes',
    '-o', 'ConnectTimeout=15',
    '-o', 'ServerAliveInterval=15',
    '-o', 'ServerAliveCountMax=8'
)

# A create that is not followed by a delete bills by the hour whether or not
# anything ever ran on it, so every path that throws after a create has to say
# what is still up - the exception on its own reads like nothing happened.
function Write-StillBilling([string[]]$Names) {
    Write-Host ""
    Write-Fail "still running and BILLING:"
    foreach ($n in $Names) { Write-Host "      hcloud server delete $n" }
    Write-Host ""
}

# Sized for route propagation rather than for boot. Hetzner allocates addresses
# out of recently acquired ranges, and a brand new one is regularly unroutable
# from a given network for several minutes - the gateway answers "Destination
# net unreachable" - before the route reaches it. Seen twice here, both times
# coming good on its own with the box perfectly healthy the whole time. A box
# that is merely booting answers in well under a minute, so nothing is lost by
# being patient with the rest.
$SshTimeout = 900
if ($cfg.ContainsKey('SSH_TIMEOUT') -and $cfg['SSH_TIMEOUT']) {
    $SshTimeout = [int]$cfg['SSH_TIMEOUT']
}

function Get-BoxName([int]$i) { return "$Gen-b$i" }

# Windows PowerShell 5.1 does not discard a native command's stderr when you
# redirect it - it wraps each line in an ErrorRecord, which $ErrorActionPreference
# = 'Stop' then treats as fatal. Both callers below have a NORMAL case that writes
# to stderr and exits non-zero ("Server not found" while checking whether a box
# exists; "connection refused" while waiting for one to boot), so a plain
# `2>$null` turned routine answers into crashes. Exit code decides here, and
# stderr comes back as a field - dropping it is what made a box that would not
# answer indistinguishable from a box with the wrong key.
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $raw = & $Exe @Arguments 2>&1
        $out = @(); $err = @()
        foreach ($line in $raw) {
            if ($line -is [System.Management.Automation.ErrorRecord]) { $err += [string]$line }
            else { $out += [string]$line }
        }
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output   = ($out -join "`n")
            Error    = ($err -join "`n")
        }
    } finally { $ErrorActionPreference = $prev }
}

function Get-BoxIp([string]$name) {
    $r = Invoke-Native (Get-Hcloud) @('server', 'ip', $name)
    if ($r.ExitCode -ne 0) { return $null }
    return $r.Output.Trim()
}

function Wait-ForSsh([string]$ip, [int]$TimeoutSec = 0) {
    # An empty address means the create failed and went unnoticed; without this
    # the loop spends the whole timeout failing to connect to "root@".
    if (-not $ip) { throw "no address to wait for - the server was never created" }

    if ($TimeoutSec -le 0) { $TimeoutSec = $SshTimeout }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    # not $args - that is an automatic variable and assigning it inside a function
    # is a good way to get a surprise later.
    # ssh takes the first value given for an option, so this 5s connect timeout
    # wins over the 15s in $BoxSsh - polling wants to fail fast and try again.
    $sshArgs = @('-o', 'ConnectTimeout=5') + $BoxSsh + @("root@$ip", 'true')
    $last = '(ssh printed nothing)'
    while ((Get-Date) -lt $deadline) {
        $r = Invoke-Native 'ssh' $sshArgs
        if ($r.ExitCode -eq 0) { return }
        $lines = @($r.Error -split "`n" | Where-Object { $_.Trim() })
        if ($lines.Count -gt 0) { $last = $lines[-1].Trim() }
        Start-Sleep -Seconds 5
    }
    # The last ssh error is the entire diagnosis and it used to be thrown away.
    # "Connection refused" is a box still booting - wait longer. "Connection
    # timed out" is nothing routing to it at all: a Hetzner firewall on the
    # project, or a server that is not really running, neither of which more
    # waiting fixes. "Permission denied" is the key: SSH_KEY names the key
    # registered with Hetzner, SSH_KEY_FILE is the private half, and they have
    # to be the same key. A timeout that survives even SSH_TIMEOUT is usually
    # still the routing case: re-running `up` retries an existing box rather
    # than making a new one, so waiting and re-running costs nothing.
    throw "$ip never became reachable over ssh after ${TimeoutSec}s. Last ssh error: $last"
}

# The scripts are copied to the box rather than taken from the clone, because
# provision.sh is what creates the clone. It also means a fix to these scripts
# reaches a running fleet without a commit.
function Push-Scripts([string]$ip) {
    & ssh @BoxSsh "root@$ip" 'mkdir -p /root/cloud /root/.ssh'
    & scp @BoxSsh -q `
        "$PSScriptRoot\common.sh" "$PSScriptRoot\provision.sh" `
        "$PSScriptRoot\run-box.sh" "$PSScriptRoot\prepare-corpus.sh" `
        "root@${ip}:/root/cloud/"
    # Always lands as job.env on the box, whatever it is called here, so the
    # remote commands below do not have to care.
    & scp @BoxSsh -q $ConfigPath "root@${ip}:/root/cloud/job.env"

    # The box has to authenticate to the hub on its own to pull corpus chunks and
    # push shards, so the key goes with it. That is a deliberate trade: this key
    # is purpose-built and reaches nothing but disposable boxes and a Storage Box
    # of chess positions. Rotate it (ssh-keygen, re-register, `fleet.ps1 up`) if a
    # box is ever suspect; do not reuse a key that opens anything else.
    if ($KeyFile) {
        & scp @BoxSsh -q $KeyFile "root@${ip}:/root/.ssh/id_ed25519"
        & ssh @BoxSsh "root@$ip" 'chmod 600 /root/.ssh/id_ed25519'
    }
    & ssh @BoxSsh "root@$ip" 'chmod +x /root/cloud/*.sh'
}

# Server-type names are not guessable and they vary by account and location -
# cpx41, cx32 and friends all look plausible and several do not exist. Check the
# name against the API before spending a minute on a create that cannot work, and
# print the real list when it fails.
function Get-ServerTypeNames {
    $r = Invoke-Native (Get-Hcloud) @('server-type', 'list', '-o', 'noheader', '-o', 'columns=name')
    if ($r.ExitCode -ne 0) { throw "cannot list server types - is HCLOUD_TOKEN valid?" }
    return @($r.Output -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Assert-ServerType([string]$type, [string]$setting) {
    if ((Get-ServerTypeNames) -contains $type) { return }
    Write-Fail "$setting=$type does not exist on this account"
    Write-Host ""
    & (Get-Hcloud) server-type list
    Write-Host ""
    throw "set $setting in job.env to one of the names above"
}

function Invoke-Types {
    Write-Section "Server types available to this account"
    & (Get-Hcloud) server-type list
    Write-Host ""
    Write-Host "  SERVER_TYPE   the fleet - dedicated vCPU (ccx*), the more cores the better"
    Write-Host "  PREPARE_TYPE  the corpus split - anything with >=20 GB disk will do"
    Write-Host ""
    Write-Host "  A type can exist and still be sold out in your LOCATION."
    Write-Host "  hcloud server-type describe <name>   shows per-location availability."
}

function New-Box([int]$i) {
    $hcloud = Get-Hcloud
    $name = Get-BoxName $i
    $existing = Get-BoxIp $name
    if ($existing) {
        Write-Ok "$name exists at $existing"
        return $existing
    }
    Write-Host "  creating $name ($($cfg['SERVER_TYPE']), $($cfg['LOCATION']))..."
    Assert-ServerType $cfg['SERVER_TYPE'] 'SERVER_TYPE'
    # Out-Host, not a bare call: a native command writes to the success stream,
    # and a function returns everything left on it. Without this, New-Box hands
    # back hcloud's own "Server N created / IPv4: ..." chatter *followed* by the
    # address, `$ips += (New-Box $i)` flattens all of it into one array, and
    # Initialize-Box then tries to ssh to root@"Server N created". calibrate and
    # prepare-corpus never showed it: their create is not inside a function whose
    # value is used.
    & $hcloud server create --name $name --type $cfg['SERVER_TYPE'] `
        --image $cfg['IMAGE'] --location $cfg['LOCATION'] --ssh-key $cfg['SSH_KEY'] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "hcloud could not create $name" }
    $ip = Get-BoxIp $name
    # Cheap, and it turns any future leak into this line rather than into five
    # minutes of waiting for ssh on something that is not an address.
    if ($ip -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
        throw "$name did not yield an IPv4 address (got '$ip')"
    }
    Write-Ok "$name at $ip"
    return $ip
}

function Initialize-Box([int]$i, [string]$ip) {
    Wait-ForSsh $ip
    Push-Scripts $ip
    Write-Host "  provisioning box $i (clone, build, acceptance gate)..."
    & ssh @BoxSsh "root@$ip" '/root/cloud/provision.sh /root/cloud/job.env'
    if ($LASTEXITCODE -ne 0) { throw "provisioning failed on box $i" }
    Write-Ok "box $i provisioned"
}

function Invoke-Up {
    # Before the first create: a fleet that cannot provision should cost nothing.
    Assert-NetReady

    Write-Section "Fleet up: $Gen, $Boxes x $($cfg['SERVER_TYPE'])"
    $ips = @()
    try {
        for ($i = 0; $i -lt $Boxes; $i++) { $ips += (New-Box $i) }
    } catch {
        # $ips is short if the throw came from a create, so name every box the
        # run asked for rather than the ones that answered.
        Write-StillBilling @(0..($Boxes - 1) | ForEach-Object { Get-BoxName $_ })
        throw
    }

    # Provision and launch one box at a time, and survive the ones that fail.
    # A single unreachable box used to abort the run after the others were
    # already provisioned and billing, which is the worst of both: paying for
    # boxes and getting no work out of them. Units are addressed by chunk with
    # done-markers on the hub, so a straggler's share is still there for a later
    # `up` to claim - nothing is lost by starting without it.
    $launched = @()
    $stalled = @()
    for ($i = 0; $i -lt $Boxes; $i++) {
        try {
            # provision.sh is idempotent by contract (see its header), so a retry
            # after a dropped connection re-verifies and exits rather than
            # rebuilding. Losing a box to one flaky ssh is not worth an hour of
            # billing.
            for ($try = 1; ; $try++) {
                try { Initialize-Box $i $ips[$i]; break }
                catch {
                    # Not for a Wait-ForSsh timeout: that already polled for
                    # SSH_TIMEOUT seconds, so retrying it just triples the wait
                    # before the box is called stalled. The transport failures
                    # this is here for fail in seconds.
                    if ($try -ge 3 -or $_.Exception.Message -match 'never became reachable') { throw }
                    Write-Warn2 "box ${i}: $($_.Exception.Message)"
                    Write-Host "  retrying box $i (attempt $($try + 1) of 3)"
                }
            }
            # systemd-run, not `setsid nohup ... &`. With the latter ssh never
            # gets EOF on the session channel and the launch call hangs forever -
            # measured, not guessed: 15s timeouts on every variant, including
            # `ssh -n`, while systemd-run returns in 2s. That hang is what
            # stalled every previous run immediately after "box N provisioned".
            # Handing the process to PID 1 also outlives the ssh session by
            # construction and gives `status` a real unit to query.
            $unit = "datagen-b$i"
            # An already-active unit is reported, not restarted: relaunching a
            # box that is working would abandon its half-finished chunk.
            # --setenv=HOME: a unit gets no HOME, and run-box.sh derives its work
            # directory from it. Belt and braces with the ${HOME:-/root} default
            # in the script, because the two have to agree with provision.sh
            # about where the clone is.
            #
            # The pgrep guard is for a job left running outside systemd - the old
            # setsid launch really did start work on the box even though the ssh
            # call hung, so a box can be busy with an unmanaged run. Starting a
            # second one would put two writers on the same shard files.
            $cmd = "systemctl is-active --quiet $unit && echo already-running || " +
                   "{ pgrep -x datagen >/dev/null && " +
                   "{ echo 'datagen already running outside the unit; refusing to start a second'; exit 1; }; " +
                   "systemctl reset-failed $unit 2>/dev/null; " +
                   "systemd-run --unit=$unit --collect --setenv=HOME=/root " +
                   "--property=WorkingDirectory=/root/cloud " +
                   "--property=StandardOutput=append:/root/run.log " +
                   "--property=StandardError=append:/root/run.log " +
                   "/root/cloud/run-box.sh $i job.env; }"
            & ssh @BoxSsh "root@$($ips[$i])" $cmd
            if ($LASTEXITCODE -ne 0) { throw "could not launch the run" }
            Write-Ok "box $i launched"
            $launched += $i
        } catch {
            Write-Warn2 "box ${i}: $($_.Exception.Message)"
            $stalled += $i
        }
    }

    if ($launched.Count -eq 0) {
        Write-StillBilling @(0..($Boxes - 1) | ForEach-Object { Get-BoxName $_ })
        throw "no box started"
    }

    if ($stalled.Count -gt 0) {
        Write-Host ""
        Write-Fail "$($stalled.Count) of $Boxes did not start:"
        foreach ($i in $stalled) { Write-Host "      $(Get-BoxName $i)" }
        Write-Host "  Their chunks are unclaimed. Re-run up in a few minutes: it reuses an"
        Write-Host "  existing box, and a new address that does not route yet usually starts"
        Write-Host "  routing on its own. Finished units are skipped by their done-markers,"
        Write-Host "  so a re-run costs nothing but the boxes you are already paying for."
    }

    Write-Host ""
    Write-Host "  $($launched.Count) of $Boxes running."
    Write-Host "  .\tools\cloud\fleet.ps1 status    progress"
    Write-Host "  .\tools\cloud\fleet.ps1 down      when it finishes - billing is hourly"
}

function Invoke-Status {
    Write-Section "Fleet status: $Gen"
    $hcloud = Get-Hcloud
    & $hcloud server list | Select-String -Pattern "^ID|$Gen-b" -SimpleMatch:$false

    # A server in state `running` does not mean the run is: a job that died an
    # hour ago leaves the box up, idle and billing, and the hub markers below
    # cannot tell "still working on the first chunk" from "stopped after five
    # seconds". Ask the box's unit.
    Write-Host ""
    Write-Host "  boxes:"
    for ($i = 0; $i -lt $Boxes; $i++) {
        $bname = Get-BoxName $i
        $bip = Get-BoxIp $bname
        if (-not $bip) { Write-Warn2 "$bname does not exist"; continue }
        # `|| true` because pgrep exits 1 when it matches nothing, which is the
        # answer being asked for rather than a failure of the probe.
        $probe = Invoke-Native 'ssh' (@('-o', 'ConnectTimeout=10') + $BoxSsh + @(
            "root@$bip",
            "systemctl is-active datagen-b$i 2>/dev/null || true; " +
            "tail -n 1 /root/run.log 2>/dev/null || echo '(no run.log)'"))
        if ($probe.ExitCode -ne 0) {
            $why = @($probe.Error -split "`n" | Where-Object { $_.Trim() })
            $msg = if ($why.Count -gt 0) { $why[-1].Trim() } else { "ssh exit $($probe.ExitCode)" }
            Write-Warn2 "${bname}: unreachable - $msg"
            continue
        }
        $lines = @($probe.Output -split "`n" | Where-Object { $_.Trim() })
        $state = if ($lines.Count -gt 0) { $lines[0].Trim() } else { 'unknown' }
        $tail = if ($lines.Count -gt 1) { $lines[-1].Trim() } else { '(no log)' }
        # `inactive` is the interesting one: it means the unit ran and exited
        # without failing, which is either "finished its chunks" or "exited 0
        # having done nothing" - the log line tells them apart.
        switch ($state) {
            'active'   { Write-Ok    "${bname}: running - $tail" }
            'failed'   { Write-Fail  "${bname}: FAILED - $tail" }
            'inactive' { Write-Warn2 "${bname}: not running - $tail" }
            default    { Write-Warn2 "${bname}: $state - $tail" }
        }
    }

    Write-Host ""
    Write-Host "  units finished (done-markers on the hub):"
    # Before the first unit finishes there is no done/ directory at all, and the
    # restricted shell makes the ls error unsuppressable from the remote side -
    # see Test-NetOnHub. It printed above the "none yet" it already means.
    $r = Invoke-Native 'ssh' ($IdentityArgs + @(
        '-p', $HubPort, '-o', 'BatchMode=yes', $HubUser, "ls $HubDir/$Gen/done/"))
    $markers = @($r.Output -split "`n" | Where-Object { $_.Trim() })
    if ($r.ExitCode -ne 0 -or $markers.Count -eq 0) {
        Write-Warn2 "none yet"
    } else {
        foreach ($m in $markers) { Write-Ok $m.Trim() }
    }
}

function Invoke-Logs {
    $ip = Get-BoxIp (Get-BoxName $Box)
    if (-not $ip) { throw "box $Box does not exist" }
    & ssh @BoxSsh "root@$ip" 'tail -n 40 /root/run.log'
}

function Invoke-Down {
    Write-Section "Fleet down: $Gen"
    $hcloud = Get-Hcloud
    for ($i = 0; $i -lt $Boxes; $i++) {
        $name = Get-BoxName $i
        if (Get-BoxIp $name) {
            & $hcloud server delete $name
            Write-Ok "deleted $name"
        }
    }
}

# One box, one measurement, then destroyed. Phase 3 of the plan: the fleet size
# comes from this number, not from an estimate.
function Invoke-Calibrate {
    Assert-NetReady

    Write-Section "Calibration: one $($cfg['SERVER_TYPE'])"
    $name = "$Gen-calib"
    $hcloud = Get-Hcloud
    $ip = Get-BoxIp $name
    if (-not $ip) {
        Assert-ServerType $cfg['SERVER_TYPE'] 'SERVER_TYPE'
        & $hcloud server create --name $name --type $cfg['SERVER_TYPE'] `
            --image $cfg['IMAGE'] --location $cfg['LOCATION'] --ssh-key $cfg['SSH_KEY']
        # Unchecked, a create that hits an account limit or a sold-out type
        # turned into five minutes of waiting for ssh on a server that does not
        # exist, reported as an ssh problem.
        if ($LASTEXITCODE -ne 0) { throw "hcloud could not create $name" }
        $ip = Get-BoxIp $name
        if (-not $ip) { throw "$name was created but has no IPv4 address" }
    }
    try {
        Wait-ForSsh $ip
        Push-Scripts $ip
        & ssh @BoxSsh "root@$ip" '/root/cloud/provision.sh /root/cloud/job.env'
        if ($LASTEXITCODE -ne 0) { throw "provisioning failed" }
        & ssh @BoxSsh "root@$ip" 'cd /root/cloud && JOB=calibrate ./run-box.sh 0 job.env'
        # Unchecked, a calibration that died on the box still fell through to the
        # delete below and reported nothing - the number you did not get is not
        # obviously missing from the output.
        if ($LASTEXITCODE -ne 0) { throw "the calibration run failed on $name" }
    } catch {
        # The box outlives the failure on purpose - the Hetzner console still
        # reaches it when ssh does not - but silently it outlives the afternoon.
        Write-StillBilling @($name)
        throw
    }

    Write-Host ""
    if ($Keep) {
        Write-Warn2 "still running and billing: $name"
        Write-Host "  destroy with: hcloud server delete $name"
    } else {
        & $hcloud server delete $name
        Write-Ok "deleted $name"
    }
}

# scp cannot create remote directories and a fresh Storage Box is empty, which
# is exactly the "dest open ... No such file or directory" you get from uploading
# straight into datagen/corpus/. Make the tree first, every time - mkdir -p is
# free when it already exists.
function Initialize-Hub {
    # One command per ssh call: the Storage Box restricted shell does not take a
    # ';'-chained list.
    & ssh @IdentityArgs -p $HubPort -o StrictHostKeyChecking=accept-new `
        $HubUser mkdir -p "$HubDir/corpus"
    if ($LASTEXITCODE -ne 0) { throw "cannot create $HubDir/corpus on the hub" }
    & ssh @IdentityArgs -p $HubPort $HubUser mkdir -p "$HubDir/nets"
    if ($LASTEXITCODE -ne 0) { throw "cannot create $HubDir/nets on the hub" }
    Write-Ok "hub tree ready: $HubDir/{corpus,nets}"
}

function Invoke-PushCorpus {
    Write-Section "Corpus -> hub"
    $archive = $Path
    if (-not $archive) { $archive = Join-Path $RepoRoot 'human.epd.zst' }
    if (-not (Test-Path $archive)) {
        throw "no $archive. Compress the corpus first:`n" +
              "    zstd -12 -T0 external\training\human.epd -o human.epd.zst"
    }
    Initialize-Hub
    $leaf = Split-Path -Leaf $archive
    $mb = [math]::Round((Get-Item $archive).Length / 1MB)
    Write-Host "  uploading $leaf ($mb MB) - this is the only large upload, ever"
    & scp @IdentityArgs -P $HubPort $archive "${HubUser}:$HubDir/corpus/$leaf"
    if ($LASTEXITCODE -ne 0) { throw "upload failed" }
    Write-Ok "uploaded"
    Write-Host "  next: .\tools\cloud\fleet.ps1 prepare-corpus"
}

function Test-NetOnHub([string]$Sha) {
    # `ls` on the file itself, not a listing of the directory: nets/ accumulates
    # a file per generation and this asks the only question that matters.
    #
    # Absent is a NORMAL answer here - a net that has not been published yet is
    # the entire reason to ask - but the hub is a Storage Box, whose restricted
    # shell parses a command and its arguments and does not honour a redirect.
    # So a `2>/dev/null` written into the remote command does nothing, and the
    # ls error travels back and prints: "cannot access ... No such file", which
    # reads exactly like a failed upload immediately before the upload starts.
    # Invoke-Native is what keeps it off the console, since a plain `2>$null`
    # on this side is what $ErrorActionPreference=Stop turns into a crash.
    $r = Invoke-Native 'ssh' ($IdentityArgs + @(
        '-p', $HubPort, '-o', 'BatchMode=yes', $HubUser, "ls $HubDir/nets/$Sha.nnue"))
    return $r.ExitCode -eq 0
}

function Get-LocalNet([string]$Sha) {
    # -Path wins; otherwise the net the exporter writes, which is where a net
    # that was just trained already is.
    $local = $Path
    if (-not $local) { $local = Join-Path $RepoRoot 'external\nets\net.nnue' }
    if (-not (Test-Path $local)) {
        throw "NET_SHA is $Sha but there is no net at $local to upload.`n" +
              "    Export one with 'make nnue-export', or pass -Path <file>."
    }

    $got = (Get-FileHash -Algorithm SHA256 -Path $local).Hash.ToLower()
    if ($got -ne $Sha.ToLower()) {
        # Named in full, both values, the way a rejected net file is. A silent
        # mismatch here is the worst class of failure this pipeline has: every
        # box would build against a net nobody chose, label a few hundred
        # million positions with it, and produce a dataset that looks perfect
        # and cannot be compared to the generation before it.
        throw "net mismatch - refusing to upload.`n" +
              "    job.env NET_SHA : $Sha`n" +
              "    $local : $got`n" +
              "    Either point NET_SHA at the net you meant, or pass -Path to the file that hashes to it."
    }
    return $local
}

# Puts the net on the hub if it is not already there. Idempotent, and called
# before anything is created - see the note on Assert-NetReady below.
function Publish-Net {
    $sha = ''
    if ($cfg.ContainsKey('NET_SHA')) { $sha = $cfg['NET_SHA'] }
    if (-not $sha) {
        throw "EVAL=nnue needs NET_SHA in job.env - it is what the boxes fetch by.`n" +
              "    Get it with: (Get-FileHash -Algorithm SHA256 external\nets\net.nnue).Hash.ToLower()"
    }

    if (Test-NetOnHub $sha) {
        Write-Ok "net $($sha.Substring(0, 12)) already on the hub"
        return
    }

    $local = Get-LocalNet $sha
    Initialize-Hub
    $mb = [math]::Round((Get-Item $local).Length / 1MB)
    Write-Host "  uploading $(Split-Path -Leaf $local) ($mb MB) as $sha.nnue"

    # Named by hash on the hub, never by the local filename. Every local net is
    # called net.nnue; the hash is the only thing that says WHICH one, and
    # provision.sh re-checks it after the download.
    & scp @IdentityArgs -P $HubPort $local "${HubUser}:$HubDir/nets/$sha.nnue"
    if ($LASTEXITCODE -ne 0) { throw "net upload failed" }

    if (-not (Test-NetOnHub $sha)) { throw "net uploaded but is not readable at $HubDir/nets/$sha.nnue" }
    Write-Ok "net $($sha.Substring(0, 12)) published"
}

<#
Preflight for anything that provisions a box.

provision.sh fetches the net and cannot supply it: it runs ON the box, and the
net is a gitignored file that exists only here. So the check has to happen on
this side - and it has to happen BEFORE a server is created, because the
alternative is what it replaced: every box in the fleet boots, installs a
toolchain, and dies on a missing net ten minutes in, having billed for all of
it. A run that cannot succeed should cost nothing.
#>
function Assert-NetReady {
    if (-not $cfg.ContainsKey('EVAL') -or $cfg['EVAL'] -ne 'nnue') { return }
    Write-Section 'Net -> hub'
    Publish-Net
}

function Invoke-PushNet {
    Write-Section 'Net -> hub'
    if (-not $cfg.ContainsKey('EVAL') -or $cfg['EVAL'] -ne 'nnue') {
        Write-Warn2 "EVAL is '$($cfg['EVAL'])', so no net is needed. Uploading anyway because you asked."
    }
    Publish-Net
}

# The split is I/O bound, not CPU bound, and needs no engine - so this box skips
# provisioning entirely and runs on a cheap shared-vCPU type with enough disk for
# the corpus plus its chunks (~25 GB).
function Invoke-PrepareCorpus {
    $type = 'cx32'
    if ($cfg.ContainsKey('PREPARE_TYPE') -and $cfg['PREPARE_TYPE']) { $type = $cfg['PREPARE_TYPE'] }
    Write-Section "Splitting the corpus on one $type"

    $name = "$Gen-prep"
    $hcloud = Get-Hcloud
    Assert-ServerType $type 'PREPARE_TYPE'
    $ip = Get-BoxIp $name
    if (-not $ip) {
        & $hcloud server create --name $name --type $type `
            --image $cfg['IMAGE'] --location $cfg['LOCATION'] --ssh-key $cfg['SSH_KEY']
        if ($LASTEXITCODE -ne 0) { throw "hcloud could not create $name" }
        $ip = Get-BoxIp $name
    }
    Write-Ok "$name at $ip"

    try {
        Wait-ForSsh $ip
        Push-Scripts $ip
    } catch {
        Write-StillBilling @($name)
        throw
    }
    Write-Host "  splitting into $($cfg['CHUNKS']) chunks (fetch, decompress, split, verify, upload)..."
    & ssh @BoxSsh "root@$ip" '/root/cloud/prepare-corpus.sh /root/cloud/job.env'
    $rc = $LASTEXITCODE

    if ($rc -ne 0) {
        Write-Fail "prepare-corpus failed; leaving $name up so you can look"
        Write-Host "  ssh -i <key> root@$ip    then: cat /root/corpus, ./cloud/prepare-corpus.sh"
        throw "prepare-corpus failed on $name"
    }

    if ($Keep) {
        Write-Warn2 "still running and billing: $name"
    } else {
        & $hcloud server delete $name
        Write-Ok "deleted $name - the chunks are on the hub"
    }
    Write-Host "  next: .\tools\cloud\fleet.ps1 calibrate"
}

switch ($Command) {
    'up'        { Invoke-Up }
    'down'      { Invoke-Down }
    'status'    { Invoke-Status }
    'logs'      { Invoke-Logs }
    'calibrate' { Invoke-Calibrate }
    'push-corpus'    { Invoke-PushCorpus }
    'push-net'       { Invoke-PushNet }
    'prepare-corpus' { Invoke-PrepareCorpus }
    'types'          { Invoke-Types }
}
