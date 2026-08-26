# aggregate.ps1 - pull a generation off the hub and turn it into one shuffled
# training file.
#
#   .\tools\cloud\aggregate.ps1                  download, check, merge, shuffle
#   .\tools\cloud\aggregate.ps1 -Parallel 4      fewer concurrent scp streams
#   .\tools\cloud\aggregate.ps1 -SkipDownload    re-merge what is already local
#   .\tools\cloud\aggregate.ps1 -StrictVerify    stop if sampled labels disagree
#
# The download is the slow part (~6.5 GB for a full human pass) and it is not
# bandwidth-bound: a Storage Box throttles per CONNECTION, so one scp saturates
# one stream and leaves the rest of the link idle. It is pulled a unit at a time
# over -Parallel connections, and every integrity check that can run before the
# shuffle runs before the shuffle.

[CmdletBinding()]
param(
    [string]$ConfigPath,
    [switch]$SkipDownload,
    [switch]$StrictVerify,
    [int]$ShuffleSeed = 7,
    [int]$Parallel = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\common.ps1"

if (-not $ConfigPath) { $ConfigPath = Join-Path $PSScriptRoot 'job.env' }

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

$cfg     = Get-JobConfig $ConfigPath
$Gen     = $cfg['GEN']
$Nodes   = $cfg['NODES']
$HubUser = $cfg['HUB']
$HubPort = $cfg['HUB_PORT']
$HubDir  = $cfg['HUB_DIR']

# Same identity discipline as fleet.ps1: the hub is reached with the purpose-built
# key, not with whatever ssh would otherwise pick.
$IdentityArgs = @()
if ($cfg.ContainsKey('SSH_KEY_FILE') -and $cfg['SSH_KEY_FILE']) {
    $k = [Environment]::ExpandEnvironmentVariables($cfg['SSH_KEY_FILE'])
    if (-not (Test-Path $k)) { throw "SSH_KEY_FILE does not exist: $k" }
    $IdentityArgs = @('-i', $k, '-o', 'IdentitiesOnly=yes')
}

$Datagen      = Join-Path $RepoRoot 'datagen.exe'
$DataDir      = Ensure-Dir (Join-Path $ExternalDir 'data')
$GenDir       = Ensure-Dir (Join-Path $DataDir $Gen)
$MergeDir     = Ensure-Dir (Join-Path $GenDir 'merged')
$ShardsRemote = "$HubDir/$Gen/shards"

$JobEval  = if ($cfg.ContainsKey('EVAL') -and $cfg['EVAL']) { $cfg['EVAL'] } else { 'classical' }
$JobArch  = if ($cfg.ContainsKey('ARCH') -and $cfg['ARCH']) { $cfg['ARCH'] } else { 'popcnt' }
$MakeLine = "make datagen EVAL=$JobEval ARCH=$JobArch"

if (-not (Test-Path $Datagen)) { throw "no datagen.exe at $Datagen - run: $MakeLine" }

# The label check below re-searches sampled positions with THIS binary and
# expects the fleet's scores back, which only holds if this binary evaluates the
# way the fleet's did. Plain `make datagen` builds the CLASSICAL eval, so a
# generation labelled by an EVAL=nnue fleet and checked by a stock local build
# mismatches on essentially every sampled record - a failure that says nothing
# whatsoever about the data. An nnue build embeds its net with .incbin and so
# cannot be smaller than the net; a classical one is a few hundred KB. That is
# the whole difference, and it is enough to tell them apart before the download.
if ($JobEval -eq 'nnue') {
    $LocalNet = Join-Path $ExternalDir 'nets\net.nnue'
    if (-not (Test-Path $LocalNet)) {
        Write-Warn2 ("no net at $LocalNet to check datagen.exe against; if labels do not " +
                     "reproduce below, suspect this build before you suspect the data")
    } elseif ((Get-Item $Datagen).Length -lt (Get-Item $LocalNet).Length) {
        Write-Warn2 ("datagen.exe carries no embedded net, but $Gen was labelled with " +
                     "EVAL=nnue - its labels cannot reproduce here. Rebuild: $MakeLine")
    } else {
        Write-Ok "datagen.exe is an nnue build, matching EVAL=$JobEval"
    }
}

# A BX11 Storage Box accepts ten simultaneous connections and each scp holds one,
# so asking for more than that does not go faster - it starts failing units.
if ($Parallel -lt 1) { $Parallel = 1 }
if ($Parallel -gt 10) {
    Write-Warn2 "-Parallel $Parallel exceeds the Storage Box connection limit; using 10"
    $Parallel = 10
}

# 64 KiB is enough to keep the copy sequential; the point is to avoid pulling a
# multi-GB shard through PowerShell's pipeline as objects.
function Join-BinaryFiles([string[]]$Inputs, [string]$Output) {
    $out = [System.IO.File]::Create($Output)
    try {
        $buf = New-Object byte[] 65536
        foreach ($f in $Inputs) {
            $in = [System.IO.File]::OpenRead($f)
            try {
                while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) { $out.Write($buf, 0, $n) }
            } finally { $in.Dispose() }
        }
    } finally { $out.Dispose() }
}

# Summed by hand rather than with Measure-Object: given -Property and no input
# objects at all, Measure-Object emits NOTHING, and `(nothing).Sum` is a
# strict-mode error - which is precisely the state a fresh generation directory
# is in on the first run, when it holds only the empty merged\ subdirectory.
function Get-DirectoryBytes([string]$Path) {
    $total = [long]0
    foreach ($f in @(Get-ChildItem -LiteralPath $Path -File -ErrorAction SilentlyContinue)) {
        $total += $f.Length
    }
    return $total
}

function Format-Rate([long]$Bytes, [double]$Seconds) {
    if ($Seconds -le 0) { return "{0} MB" -f [Math]::Round($Bytes / 1MB) }
    return "{0} MB in {1}s ({2} MB/s)" -f [Math]::Round($Bytes / 1MB),
                                          [Math]::Round($Seconds),
                                          [Math]::Round($Bytes / 1MB / $Seconds, 1)
}

# `ls -l` rows -> name and size. Counting the columns back from the name survives
# the busybox-ish variant that omits the group, and a row that does not parse is
# dropped rather than guessed at - a wrong size here would skip a real unit.
function ConvertFrom-LsListing([string[]]$Lines) {
    $files = @()
    foreach ($line in $Lines) {
        $t = "$line".Trim()
        if ($t -eq '' -or $t -match '^total\b') { continue }
        $f = @($t -split '\s+')
        if ($f.Count -ge 5 -and $f[-1] -match '\.(cnn|pol)$' -and $f[-5] -match '^\d+$') {
            $files += [pscustomobject]@{ Name = $f[-1]; Size = [long]$f[-5] }
        }
    }
    return $files
}

# One ssh round trip for the whole listing. `-l` because the sizes are what make
# the download restartable: a unit already on disk at the right length is skipped,
# and everything that is pulled is checked against the length the hub reported. A
# hub whose shell will not do `-l` still works, it just loses both (Size = -1).
function Get-RemoteShards {
    $listing = & ssh @IdentityArgs -p $HubPort -o BatchMode=yes $HubUser "ls -l $ShardsRemote"
    if ($LASTEXITCODE -ne 0) { throw "cannot list ${HubUser}:$ShardsRemote" }

    $files = @(ConvertFrom-LsListing $listing)
    if ($files.Count -gt 0) { return $files }

    Write-Warn2 "hub gave no parseable 'ls -l'; falling back to names only"
    $listing = & ssh @IdentityArgs -p $HubPort -o BatchMode=yes $HubUser "ls $ShardsRemote"
    if ($LASTEXITCODE -ne 0) { throw "cannot list ${HubUser}:$ShardsRemote" }
    foreach ($line in $listing) {
        $t = "$line".Trim()
        if ($t -match '\.(cnn|pol)$') { $files += [pscustomobject]@{ Name = $t; Size = [long]-1 } }
    }
    if ($files.Count -eq 0) { throw "no .cnn/.pol files in ${HubUser}:$ShardsRemote" }
    return $files
}

# The unit - one labelled chunk or one selfplay batch - is the download's work
# item as well as the merge's, because it is the largest thing a single glob can
# name. A unit is refetched whole if any of its files is missing or the wrong
# length: re-pulling a few hundred MB on a retry is cheaper than a connection per
# file, and when sizes are unknown nothing on disk is assumed complete.
function Group-ShardsIntoUnits([object[]]$Remote, [string]$LocalDir) {
    $units = @()
    foreach ($g in ($Remote | Group-Object { $_.Name -replace '_\d+\.(cnn|pol)$', '' })) {
        $bytes = [long]0
        $need  = $false
        foreach ($r in $g.Group) {
            if ($r.Size -lt 0) { $need = $true; continue }
            $bytes += $r.Size
            $local = Join-Path $LocalDir $r.Name
            if (-not (Test-Path -LiteralPath $local)) { $need = $true; continue }
            if ((Get-Item -LiteralPath $local).Length -ne $r.Size) { $need = $true }
        }
        $units += [pscustomobject]@{
            Name = $g.Name; Files = $g.Count; Bytes = $bytes; Need = $need
        }
    }
    # Biggest first. With uneven units, starting the long one last is exactly what
    # leaves a single connection grinding away on its own at the end.
    return @($units | Sort-Object -Property @{ Expression = 'Bytes'; Descending = $true })
}

# One scp per unit, one glob per scp. Several remote paths in a single scp call is
# not portable - since OpenSSH 9 scp speaks SFTP and expands globs itself instead
# of handing the string to a remote shell - and one scp per FILE pays a key
# exchange per file, which across ~400 files costs more than concurrency buys.
$FetchUnit = {
    param($Unit)
    Start-Job -ArgumentList @(@{
        ScpArgs = @($IdentityArgs + @('-P', $HubPort, '-q'))
        Source  = "${HubUser}:$ShardsRemote/$($Unit.Name)_*"
        Dest    = $GenDir
    }) -ScriptBlock {
        param($a)
        # Continue, not Stop: scp's stderr arrives through 2>&1 as ErrorRecords and
        # would end the job before its exit code could be read.
        $ErrorActionPreference = 'Continue'
        $argv = @($a.ScpArgs) + @($a.Source, $a.Dest)
        $log  = & scp @argv 2>&1 | ForEach-Object { "$_" }
        @{ Ok = ($LASTEXITCODE -eq 0); Log = ($log -join "`n") }
    }
}

# Keeps $Parallel jobs in flight until the queue drains. Returns the names of the
# units that failed rather than throwing, because the caller retries them.
function Invoke-ParallelFetch {
    param([object[]]$Units, [int]$Parallel, [scriptblock]$Launcher, [string]$WatchDir)

    $queue = New-Object System.Collections.Queue
    foreach ($u in $Units) { $queue.Enqueue($u) | Out-Null }

    $expected = [long]0
    foreach ($u in $Units) { $expected += $u.Bytes }
    $baseline = Get-DirectoryBytes $WatchDir
    $sw       = [Diagnostics.Stopwatch]::StartNew()
    $active   = @()
    $failed   = @()
    $beat     = 0

    try {
        while ($queue.Count -gt 0 -or $active.Count -gt 0) {
            while ($queue.Count -gt 0 -and $active.Count -lt $Parallel) {
                $u = $queue.Dequeue()
                $active += [pscustomobject]@{
                    Unit = $u; Job = (& $Launcher $u); Sw = [Diagnostics.Stopwatch]::StartNew()
                }
            }

            Start-Sleep -Milliseconds 500

            $still = @()
            foreach ($a in $active) {
                if ($a.Job.State -eq 'Running' -or $a.Job.State -eq 'NotStarted') {
                    $still += $a
                    continue
                }
                $state = $a.Job.State
                $out   = @(Receive-Job $a.Job -ErrorAction SilentlyContinue)
                Remove-Job $a.Job -Force
                $a.Sw.Stop()

                $payload = @($out | Where-Object { $_ -is [hashtable] })
                if ($state -eq 'Completed' -and $payload.Count -gt 0 -and $payload[-1].Ok) {
                    Write-Ok ("{0}: {1} files, {2}" -f $a.Unit.Name, $a.Unit.Files,
                              (Format-Rate $a.Unit.Bytes $a.Sw.Elapsed.TotalSeconds))
                } else {
                    $why = if ($payload.Count -gt 0) { $payload[-1].Log } else { "job $state" }
                    Write-Fail "$($a.Unit.Name): $why"
                    $failed += $a.Unit.Name
                }
            }
            $active = $still

            # A unit runs for minutes; without this the console looks wedged.
            if ($active.Count -gt 0 -and $sw.Elapsed.TotalSeconds -ge $beat + 30) {
                $beat = [Math]::Floor($sw.Elapsed.TotalSeconds)
                # A refetch overwrites in place, so this dips below the baseline
                # for as long as scp has the file truncated.
                $got = [Math]::Max([long]0, (Get-DirectoryBytes $WatchDir) - $baseline)
                # $expected is 0 only on the no-sizes fallback; claiming "of 0 MB"
                # there would read as a bug rather than as a missing number.
                $of = if ($expected -gt 0) {
                    " of {0} MB ({1:N0}%)" -f [Math]::Round($expected / 1MB), (100.0 * $got / $expected)
                } else { '' }
                Write-Host ("         {0} MB{1}, {2} streams, {3}" -f
                            [Math]::Round($got / 1MB), $of, $active.Count,
                            (Format-Rate $got $sw.Elapsed.TotalSeconds))
            }
        }
    } finally {
        # Ctrl-C lands here. An orphaned job holds one of the ten connections the
        # box allows, and the next run would then fail for no visible reason.
        foreach ($a in $active) {
            Stop-Job   $a.Job -ErrorAction SilentlyContinue
            Remove-Job $a.Job -Force -ErrorAction SilentlyContinue
        }
    }

    $sw.Stop()
    return $failed
}

Write-Section "Aggregating $Gen"

if (-not $SkipDownload) {
    $remote = @(Get-RemoteShards)
    $units  = @(Group-ShardsIntoUnits $remote $GenDir)
    $todo   = @($units | Where-Object { $_.Need })
    $have   = $units.Count - $todo.Count

    Write-Host ("  hub has {0} files in {1} units{2}" -f $remote.Count, $units.Count,
                $(if ($have -gt 0) { "; $have already complete on disk" } else { '' }))

    if ($todo.Count -gt 0) {
        $streams = [Math]::Min($Parallel, $todo.Count)
        Write-Host ("  downloading {0} units over {1} connections -> {2}" -f
                    $todo.Count, $streams, $GenDir)

        $pending = $todo
        for ($attempt = 1; $attempt -le 2 -and $pending.Count -gt 0; $attempt++) {
            if ($attempt -gt 1) { Write-Warn2 "retrying $($pending.Count) unit(s)" }
            $bad = @(Invoke-ParallelFetch -Units $pending -Parallel $streams `
                                          -Launcher $FetchUnit -WatchDir $GenDir)
            $pending = @($pending | Where-Object { $bad -contains $_.Name })
        }
        if ($pending.Count -gt 0) {
            throw "download failed for: $(($pending | ForEach-Object { $_.Name }) -join ', ')"
        }
    }

    # The concurrency is what makes this worth checking: a stream that dies
    # mid-file leaves a short one behind, and a short .pol is a quietly
    # mislabelled dataset rather than an error.
    $bad = @()
    foreach ($r in $remote) {
        $local = Join-Path $GenDir $r.Name
        if (-not (Test-Path -LiteralPath $local)) { $bad += "$($r.Name): missing"; continue }
        $len = (Get-Item -LiteralPath $local).Length
        if ($r.Size -ge 0 -and $len -ne $r.Size) {
            $bad += "$($r.Name): $len bytes local, $($r.Size) on the hub"
        }
    }
    if ($bad.Count -gt 0) {
        throw "$($bad.Count) file(s) do not match the hub:`n    " +
              (($bad | Select-Object -First 5) -join "`n    ")
    }
    Write-Ok "$($remote.Count) files match the hub byte for byte"
}

$shards = @(Get-ChildItem -Path $GenDir -Filter '*.cnn' -File | Sort-Object Name)
if ($shards.Count -eq 0) { throw "no shards in $GenDir" }
Write-Ok "$($shards.Count) shards on disk"

# A shard whose .pol is missing does not fail the shuffle - it makes datagen
# drop the policy sidecar for the ENTIRE dataset (datagen.c:2145-2154). That is
# a silent quality loss across every record, so it is checked here and treated
# as fatal.
Write-Host "  checking record/policy alignment..."
$totalRecords = 0
foreach ($s in $shards) {
    $pol = [System.IO.Path]::ChangeExtension($s.FullName, '.pol')
    if (-not (Test-Path $pol)) { throw "$($s.Name) has no .pol sidecar; refusing to shuffle" }
    if ($s.Length % 32 -ne 0) { throw "$($s.Name) is not a whole number of 32-byte records" }
    $recs = $s.Length / 32
    $prec = (Get-Item $pol).Length / 4
    if ($recs -ne $prec) { throw "$($s.Name): $recs records but $prec policy entries" }
    $totalRecords += $recs
}
Write-Ok "$totalRecords records, sidecars aligned"

# Merge per chunk/batch before shuffling. 8 inputs instead of ~400 keeps the
# shuffle command line well inside the Windows limit, and .cnn/.pol concatenate
# byte-for-byte as long as both are joined in the SAME order - hence the single
# sorted list driving both.
Write-Host "  merging by unit..."
Get-ChildItem $MergeDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
$groups = $shards | Group-Object { $_.BaseName -replace '_\d+$', '' }
foreach ($g in $groups) {
    $ordered = @($g.Group | Sort-Object Name)
    $cnnOut = Join-Path $MergeDir "$($g.Name).cnn"
    $polOut = Join-Path $MergeDir "$($g.Name).pol"
    Join-BinaryFiles ($ordered | ForEach-Object { $_.FullName }) $cnnOut
    Join-BinaryFiles ($ordered | ForEach-Object {
        [System.IO.Path]::ChangeExtension($_.FullName, '.pol') }) $polOut
    Write-Ok "$($g.Name): $($ordered.Count) shards"
}

# Relabelling a sample from a cleared engine is what catches a box whose build
# drifted from the rest of the fleet - and, at least as often, a LOCAL build that
# drifted from the fleet's. It is a check on the labels, not on the bytes, and by
# this point the bytes are already known good, so a mismatch is reported as
# loudly as it can be and aggregation continues. -StrictVerify makes it fatal
# again, for when the fleet itself is what is under suspicion.
#
# -relabel is given the -nodes AND the -hash the pass actually used, both read
# from the shard's own manifest rather than from job.env, which can have moved on
# since the generation was produced. Nodes are the one that bites: a re-search at
# a different node count disagrees with almost every label. Hash size is harmless
# at these counts - a 10k-node search from a cleared table never fills even 8 MB,
# and 8 MB against 128 MB was measured at 0 differences in 256 - but it costs
# nothing to pass it and it removes the question from the list of suspects.
$sampleFile = Get-ChildItem $MergeDir -Filter '*.cnn' | Select-Object -First 1
$sample     = $sampleFile.FullName

$vNodes = [int]$Nodes
$vHash  = 8
$manifest = Get-ChildItem $GenDir -Filter "$($sampleFile.BaseName)_*.json" | Select-Object -First 1
if ($manifest) {
    $m  = ConvertFrom-Json (Get-Content -LiteralPath $manifest.FullName -Raw)
    $mf = $m.PSObject.Properties.Name
    if ($mf -contains 'nodes')   { $vNodes = [int]$m.nodes }
    if ($mf -contains 'hash_mb') { $vHash  = [int]$m.hash_mb }
    if ($vNodes -ne [int]$Nodes) {
        Write-Warn2 ("job.env says NODES=$Nodes but $($manifest.Name) was labelled at " +
                     "$vNodes; relabelling at $vNodes")
    }
} else {
    Write-Warn2 ("no manifest beside $($sampleFile.BaseName); relabelling at NODES=$Nodes and " +
                 "a $vHash MB hash, either of which may not be what the pass used")
}

Write-Host "  verifying labels reproduce (relabel 256 @ $vNodes nodes, $vHash MB hash)..."

# datagen names every mismatching record on stderr, and that is the useful half
# of the output. Run the whole script under `2>&1 | Tee-Object` - which is what
# anyone babysitting a multi-hour aggregation does - and PowerShell turns each of
# those lines into an ErrorRecord, which $ErrorActionPreference = 'Stop' then
# raises as a NativeCommandError. The script would die at exactly the point it is
# supposed to keep going and explain itself, so the preference is lifted for this
# one call and $LASTEXITCODE is trusted instead.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $verifyOut  = @(& $Datagen verify $sample -relabel 256 -nodes $vNodes -hash $vHash)
    $verifyExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevEap
}
$verifyOut | ForEach-Object { Write-Host "    $_" }

$rtBad = $null; $polBad = 0; $labelBad = $null; $labelChecked = 0
foreach ($line in $verifyOut) {
    if     ($line -match '^round-trip:.*checked, (\d+) failures') { $rtBad  = [int]$Matches[1] }
    elseif ($line -match '^policy:.*checked, (\d+) not legal')    { $polBad = [int]$Matches[1] }
    elseif ($line -match '^relabel:\s+(\d+) checked, (\d+) mismatch') {
        $labelChecked = [int]$Matches[1]
        $labelBad     = [int]$Matches[2]
    }
}

# A verify that printed no summary did not run - a missing file, a rejected
# option, a crash - and its silence must not be read as a pass.
if ($null -eq $rtBad -or $null -eq $labelBad) {
    throw "datagen verify produced no result for $sample (exit $verifyExit)"
}

# A record that does not round-trip, or a policy move that is not legal in its
# own record, is corruption rather than disagreement: the shuffle would carry it
# straight into the training file. Those stay fatal.
if ($rtBad -gt 0 -or $polBad -gt 0) {
    throw "$sample is corrupt: $rtBad records do not round-trip, " +
          "$polBad policy moves are not legal in their record"
}

if ($labelBad -eq 0) {
    Write-Ok "$labelChecked sampled labels reproduce exactly"
} else {
    Write-Host ''
    Write-Fail "$labelBad of $labelChecked sampled labels did not reproduce"
    Write-Host "  The records are intact - they round-trip and their policy moves are legal."
    Write-Host "  What does not reproduce is the SCORE, re-searched here at $vNodes nodes and"
    Write-Host "  $vHash MB of hash. This engine is not the engine that labelled $Gen."
    Write-Host ''
    Write-Host '  In order of likelihood:'
    Write-Host '    1. this datagen.exe was not built the way the fleet built its own. The job'
    Write-Host "       asked for EVAL=$JobEval ARCH=$JobArch; rebuild with: $MakeLine"
    if ($JobEval -eq 'nnue') {
        $sha = if ($cfg.ContainsKey('NET_SHA') -and $cfg['NET_SHA']) { $cfg['NET_SHA'] }
                        else { '(unset)' }
        Write-Host "       and with the pinned net in place first - NET_SHA $sha"
    }
    Write-Host '    2. the fleet disagreed with itself. Every box verifies its own unit before'
    Write-Host '       uploading it, so that should already have failed there - read the logs.'
    Write-Host ''
    Write-Host '  Case 1 costs nothing and the shuffled file is fine. Case 2 means the dataset'
    Write-Host '  mixes labels from two different engines, which is precisely what this check'
    Write-Host '  exists to catch - do not train on it until you know which one you have.'
    Write-Host ''
    if ($StrictVerify) { throw "label verification failed on $sample (-StrictVerify)" }
    Write-Warn2 'aggregating anyway; -StrictVerify -SkipDownload reruns this as a hard gate'
}

$outFile = Join-Path $DataDir "$Gen.cnn"
$inputs  = @(Get-ChildItem $MergeDir -Filter '*.cnn' | ForEach-Object { $_.FullName })
Write-Host "  shuffling $($inputs.Count) merged files -> $outFile"
& $Datagen shuffle @inputs -o $outFile -seed $ShuffleSeed
if ($LASTEXITCODE -ne 0) { throw "shuffle failed" }

Write-Section "Result"
& $Datagen stats $outFile

Write-Host ""
Write-Host "  train with:"
Write-Host "    python -m nnue.train --train external\data\$Gen.cnn"
Write-Host "  record this generation in docs\EXPERIMENTS.md: size, nodes, eval, commit."
