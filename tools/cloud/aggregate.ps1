# aggregate.ps1 - pull a generation off the hub and turn it into one shuffled
# training file.
#
#   .\tools\cloud\aggregate.ps1                  download, check, shuffle
#   .\tools\cloud\aggregate.ps1 -Parallel 4      fewer concurrent scp streams
#   .\tools\cloud\aggregate.ps1 -SkipDownload    re-shuffle what is already local
#   .\tools\cloud\aggregate.ps1 -StrictVerify    stop if sampled labels disagree
#   .\tools\cloud\aggregate.ps1 -DataDir F:\data  shards and result somewhere other than external\data
#
# The download is not bandwidth-bound: a Storage Box throttles per CONNECTION,
# so one scp saturates one stream and leaves the rest of the link idle. It is
# pulled a unit at a time over -Parallel connections, and every integrity check
# that can run before the shuffle runs before the shuffle.
#
# The shards go into `datagen shuffle` as they are. There used to be a merge
# step here that concatenated each unit into one file first, because a thousand
# paths do not fit on a Windows command line - it cost a full second copy of the
# generation, written and read back for nothing, and it dropped every shard's
# manifest on the floor, which is why gen-005.json says `"nodes": 0`. `-inputs`
# takes the list in a file instead, and the shuffled output's record order is
# unchanged: the merge only ever concatenated in the order the shards are passed
# in now.
#
# Interrupting this is safe and leaves nothing that lies about itself - see
# "what a Ctrl-C leaves behind" at the bottom.

[CmdletBinding()]
param(
    [string]$ConfigPath,
    [switch]$SkipDownload,
    [switch]$StrictVerify,
    [int]$ShuffleSeed = 7,
    [int]$Parallel = 8,
    [string]$DataDir
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
# Resolved to a full path up front: the merge writes through [System.IO.File],
# which resolves a relative path against the PROCESS directory, not PowerShell's
# current location.
if (-not $DataDir) { $DataDir = Join-Path $ExternalDir 'data' }
$DataDir      = Ensure-Dir $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DataDir)
$GenDir       = Ensure-Dir (Join-Path $DataDir $Gen)
# The shuffle's bucket files, in one directory this script owns, so an interrupted
# run leaves its temporaries somewhere known and the next run can sweep them. Left
# to itself datagen puts them beside -o as <out>.bucketNNN.tmp, where a run killed
# in pass 2 strands up to a whole generation of them among the datasets.
$TmpDir       = Join-Path $GenDir 'shuffle-tmp'
$ShardsRemote = "$HubDir/$Gen/shards"
$DoneRemote   = "$HubDir/$Gen/done"

$JobEval  = if ($cfg.ContainsKey('EVAL') -and $cfg['EVAL']) { $cfg['EVAL'] } else { 'nnue' }
$JobArch  = if ($cfg.ContainsKey('ARCH') -and $cfg['ARCH']) { $cfg['ARCH'] } else { 'popcnt' }
$MakeLine = "make datagen EVAL=$JobEval ARCH=$JobArch"

if (-not (Test-Path $Datagen)) { throw "no datagen.exe at $Datagen - run: $MakeLine" }

# The label check below re-searches sampled positions with THIS binary and
# expects the fleet's scores back, which only holds if this binary evaluates the
# way the fleet's did. Plain `make datagen` builds the DEFAULT eval, which is the
# NETWORK - so the direction of the mistake has flipped: a stock local build now
# reproduces an EVAL=nnue fleet and NOT an EVAL=classical one. Either way the
# mismatch lands on essentially every sampled record, a failure that says
# nothing whatsoever about the data. An nnue build embeds its net with .incbin
# and so cannot be smaller than the net; a classical one is a few hundred KB.
# That is the whole difference, and it is enough to tell them apart before the
# download.
$LocalNet = Join-Path $ExternalDir 'nets\net.nnue'
if (-not (Test-Path $LocalNet)) {
    Write-Warn2 ("no net at $LocalNet to size datagen.exe against; if labels do not " +
                 "reproduce below, suspect this build before you suspect the data")
} else {
    $CarriesNet = (Get-Item $Datagen).Length -ge (Get-Item $LocalNet).Length
    if ($JobEval -eq 'nnue' -and -not $CarriesNet) {
        Write-Warn2 ("datagen.exe carries no embedded net, but $Gen was labelled with " +
                     "EVAL=nnue - its labels cannot reproduce here. Rebuild: $MakeLine")
    } elseif ($JobEval -ne 'nnue' -and $CarriesNet) {
        Write-Warn2 ("datagen.exe embeds a net - the default build does - but $Gen was " +
                     "labelled with EVAL=$JobEval. Its labels cannot reproduce here. " +
                     "Rebuild: $MakeLine")
    } else {
        Write-Ok "datagen.exe matches EVAL=$JobEval"
    }
}

# A BX11 Storage Box accepts ten simultaneous connections and each scp holds one,
# so asking for more than that does not go faster - it starts failing units.
if ($Parallel -lt 1) { $Parallel = 1 }
if ($Parallel -gt 10) {
    Write-Warn2 "-Parallel $Parallel exceeds the Storage Box connection limit; using 10"
    $Parallel = 10
}

# Summed by hand rather than with Measure-Object: given -Property and no input
# objects at all, Measure-Object emits NOTHING, and `(nothing).Sum` is a
# strict-mode error - which is precisely the state a fresh generation directory
# is in on the first run, before the first unit has landed in it.
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

# ssh writes its diagnostics to stderr, and under `2>&1 | Tee-Object` - which is
# how anyone babysitting a long aggregation runs this - PowerShell turns each of
# those lines into an ErrorRecord that $ErrorActionPreference = 'Stop' raises as
# a NativeCommandError. Every one of these calls has a considered answer for a
# hub that will not talk, and none of them get to give it if the shell kills the
# script first. The exit code is the thing to read.
function Invoke-Hub([string]$Command) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & ssh @IdentityArgs -p $HubPort -o BatchMode=yes $HubUser $Command 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    # The ErrorRecords are ssh's stderr; they are not listing lines and must not be
    # parsed as any. They are kept for the failure message and dropped otherwise.
    return [pscustomobject]@{
        Code  = $code
        Lines = @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] })
        Error = (@($out | Where-Object { $_ -is [System.Management.Automation.ErrorRecord] }) |
                 ForEach-Object { "$_" }) -join '; '
    }
}

# One ssh round trip for the whole listing. `-l` because the sizes are what make
# the download restartable: a unit already on disk at the right length is skipped,
# and everything that is pulled is checked against the length the hub reported. A
# hub whose shell will not do `-l` still works, it just loses both (Size = -1).
function Get-RemoteShards {
    $r = Invoke-Hub "ls -l $ShardsRemote"
    if ($r.Code -ne 0) { throw "cannot list ${HubUser}:${ShardsRemote}: $($r.Error)" }
    $listing = $r.Lines

    $files = @(ConvertFrom-LsListing $listing)
    if ($files.Count -gt 0) { return $files }

    Write-Warn2 "hub gave no parseable 'ls -l'; falling back to names only"
    $r = Invoke-Hub "ls $ShardsRemote"
    if ($r.Code -ne 0) { throw "cannot list ${HubUser}:${ShardsRemote}: $($r.Error)" }
    $listing = $r.Lines
    foreach ($line in $listing) {
        $t = "$line".Trim()
        if ($t -match '\.(cnn|pol)$') { $files += [pscustomobject]@{ Name = $t; Size = [long]-1 } }
    }
    if ($files.Count -eq 0) { throw "no .cnn/.pol files in ${HubUser}:$ShardsRemote" }
    return $files
}

# A unit is COMPLETE only once its done-marker exists. hub_put uploads with
# rsync --partial --inplace, so a shard still being written sits at its FINAL
# name with a partial size - and the download's length checks agree with that
# size, having read it from the same listing. run-box.sh writes
# $GEN/done/<unit>.done only after every file of a unit is up, so the marker is
# the one thing separating "finished" from "in flight". Without consulting it,
# a run against a live fleet quietly merges a fraction of the generation.
function Get-DoneUnits {
    $r = Invoke-Hub "ls $DoneRemote"
    if ($r.Code -ne 0) {
        # A generation older than the markers. Refusing would be worse than
        # merging: that data is genuinely finished, it just never said so.
        Write-Warn2 "no $DoneRemote on the hub; merging every shard present"
        if ($r.Error) { Write-Host "         ssh said: $($r.Error)" }
        return $null
    }
    $done = @{}
    foreach ($line in $r.Lines) {
        $t = "$line".Trim()
        if ($t -match '\.done$') { $done[($t -replace '\.done$', '')] = $true }
    }
    return $done
}

# Drops every file whose unit has no marker. The unit name uses the same
# expression Group-ShardsIntoUnits does and matches the marker exactly:
# run-box.sh builds the shard names and the marker from one stem.
function Select-CompleteUnits([object[]]$Files, $Done) {
    if ($null -eq $Done) { return $Files }
    return @($Files | Where-Object {
        $Done.ContainsKey(($_.Name -replace '_\d+\.(cnn|pol)$', ''))
    })
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

# Read once; both the download and the local merge filter on it.
$DoneUnits = Get-DoneUnits
if ($null -ne $DoneUnits) {
    $expected = 0
    if ($cfg.ContainsKey('JOB') -and $cfg['JOB'] -eq 'selfplay') {
        if ($cfg.ContainsKey('SELFPLAY_BATCHES')) { $expected = [int]$cfg['SELFPLAY_BATCHES'] }
    } elseif ($cfg.ContainsKey('CHUNKS')) {
        $expected = [int]$cfg['CHUNKS']
    }
    $of = if ($expected -gt 0) { " of $expected" } else { '' }
    Write-Host "  $($DoneUnits.Count) completed unit(s)$of on the hub"
    if ($DoneUnits.Count -eq 0) {
        throw "no completed units in $DoneRemote - the fleet has not finished one yet"
    }
    if ($expected -gt 0 -and $DoneUnits.Count -lt $expected) {
        Write-Warn2 ("merging a PARTIAL generation: {0} of {1} units. The rest are still " +
                     "in flight and are skipped, not truncated." -f $DoneUnits.Count, $expected)
    }
}

if (-not $SkipDownload) {
    $remote = @(Select-CompleteUnits (Get-RemoteShards) $DoneUnits)
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
# -SkipDownload arrives here with whatever an earlier run left on disk, which
# can include a unit that was in flight at the time. Filter locally as well.
$shards = @(Select-CompleteUnits $shards $DoneUnits)
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

# The shuffle's input list. Sorted by name, which is the order the old merge step
# concatenated in, so the record stream the shuffle sees - and therefore its
# output, byte for byte - is the one this pipeline has always produced.
$ListFile = Join-Path $GenDir 'shuffle-inputs.txt'
Write-TextNoBom $ListFile (($shards | ForEach-Object { $_.FullName }) -join "`r`n")
$units = @($shards | Group-Object { $_.BaseName -replace '_\d+$', '' }).Count
Write-Ok "$($shards.Count) shards in $units unit(s) listed for the shuffle"

# Anything left by a run that was killed: bucket files, from before -tmp pointed
# them at a directory of their own, and the merged copies the merge step used to
# make. Both are a whole generation in size and neither is read any more.
$legacy = @()
$legacy += @(Get-ChildItem $DataDir -Filter "$Gen.cnn.bucket*.tmp" -File -ErrorAction SilentlyContinue)
$legacyMerge = Join-Path $GenDir 'merged'
if (Test-Path $legacyMerge) { $legacy += @(Get-ChildItem $legacyMerge -File -ErrorAction SilentlyContinue) }
if ($legacy.Count -gt 0) {
    $freed = [long]0
    foreach ($f in $legacy) { $freed += $f.Length }
    Write-Host ("  reclaiming {0:N0} MB from {1} stale file(s) (merged copies, old bucket temporaries)" -f
                ($freed / 1MB), $legacy.Count)
    $legacy | Remove-Item -Force -ErrorAction SilentlyContinue
    if (Test-Path $legacyMerge) { Remove-Item $legacyMerge -Force -Recurse -ErrorAction SilentlyContinue }
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
# since the generation was produced. Nodes are the one that bites hardest: a
# re-search at a different node count disagrees with almost every label.
#
# Hash size was once believed harmless here - "8 MB against 128 MB, 0 differences
# in 256" - and that belief was wrong. 256 samples at the true rate of roughly one
# in a thousand finds nothing about three times in four, so the measurement never
# had the power to see what it concluded. gen-004 found it the expensive way: the
# fleet's own gate defaulted to 8 MB against selfplay's 64 and failed two boxes,
# twice each, on one sampled record out of 256 after hours of good selfplay.
# search_clear() empties the table but keeps its geometry, so the table size
# decides which entries collide, and a collision that changes a score is rare
# rather than impossible.
# One shard from each of up to four DIFFERENT units, rather than 256 records out
# of one. The check exists to catch one box whose build drifted from the rest of
# the fleet, and a sample that never leaves one box's output cannot see that. The
# budget is the same 256 re-searches, and verify's other two checks - which read
# every record of what they are given - now cover four 18 MB shards instead of
# one 2 GB merge, which is also the faster way round.
$byUnit  = @($shards | Group-Object { $_.BaseName -replace '_\d+$', '' })
$samples = @($byUnit | ForEach-Object { $_.Group[0] } | Select-Object -First 4)
$perFile = [Math]::Max(1, [int](256 / $samples.Count))

$labelChecked = 0
$labelBad     = 0
$vNodes       = [int]$Nodes
$vHash        = 8

foreach ($sampleFile in $samples) {
    $sample = $sampleFile.FullName
    $vNodes = [int]$Nodes
    $vHash  = 8
    # A self-play score is what the game's own search returned, with a warm table
    # and a deeper tree behind it, so a cleared engine cannot reproduce it and
    # datagen refuses -relabel on such a shard outright - "a gate that always
    # fails is worse than no gate". The manifest says which kind this is. While
    # the sample was a merged file with no manifest, nothing here could tell, and
    # a selfplay generation got 256 re-searches whose mismatches meant nothing.
    $canRelabel = $true

    # Now that the sample is a shard rather than a merge, its manifest is the file
    # beside it rather than a glob - and verify would read it on its own. It is
    # still read here, because job.env having moved on since the pass is worth a
    # warning and verify has no way to know that.
    $manifest = [System.IO.Path]::ChangeExtension($sample, '.json')
    if (Test-Path -LiteralPath $manifest) {
        $m  = ConvertFrom-Json (Get-Content -LiteralPath $manifest -Raw)
        $mf = $m.PSObject.Properties.Name
        if ($mf -contains 'nodes')   { $vNodes = [int]$m.nodes }
        if ($mf -contains 'hash_mb') { $vHash  = [int]$m.hash_mb }
        if ($mf -contains 'labels' -and $m.labels -eq 'game') { $canRelabel = $false }
        if ($canRelabel -and $vNodes -ne [int]$Nodes) {
            Write-Warn2 ("job.env says NODES=$Nodes but $($sampleFile.BaseName).json was " +
                         "labelled at $vNodes; relabelling at $vNodes")
        }
    } else {
        Write-Warn2 ("no manifest beside $($sampleFile.BaseName); relabelling at NODES=$Nodes " +
                     "and a $vHash MB hash, either of which may not be what the pass used")
    }

    if ($canRelabel) {
        Write-Host ("  verifying $($sampleFile.Name) (relabel $perFile @ $vNodes nodes, " +
                    "$vHash MB hash)...")
    } else {
        Write-Host "  verifying $($sampleFile.Name) (bytes only - game labels)..."
    }

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
        $verifyOut = if ($canRelabel) {
            @(& $Datagen verify $sample -relabel $perFile -nodes $vNodes -hash $vHash)
        } else {
            @(& $Datagen verify $sample)
        }
        $verifyExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    $verifyOut | ForEach-Object { Write-Host "    $_" }

    $rtBad = $null; $polBad = 0; $fileBad = $null; $fileChecked = 0
    foreach ($line in $verifyOut) {
        if     ($line -match '^round-trip:.*checked, (\d+) failures') { $rtBad  = [int]$Matches[1] }
        elseif ($line -match '^policy:.*checked, (\d+) not legal')    { $polBad = [int]$Matches[1] }
        elseif ($line -match '^relabel:\s+(\d+) checked, (\d+) mismatch') {
            $fileChecked = [int]$Matches[1]
            $fileBad     = [int]$Matches[2]
        }
    }

    # A verify that printed no summary did not run - a missing file, a rejected
    # option, a crash - and its silence must not be read as a pass.
    if ($null -eq $rtBad -or ($canRelabel -and $null -eq $fileBad)) {
        throw "datagen verify produced no result for $sample (exit $verifyExit)"
    }

    # A record that does not round-trip, or a policy move that is not legal in its
    # own record, is corruption rather than disagreement: the shuffle would carry it
    # straight into the training file. Those stay fatal.
    if ($rtBad -gt 0 -or $polBad -gt 0) {
        throw "$sample is corrupt: $rtBad records do not round-trip, " +
              "$polBad policy moves are not legal in their record"
    }

    if ($canRelabel) {
        $labelChecked += $fileChecked
        $labelBad     += $fileBad
    }
}

if ($labelChecked -eq 0) {
    # Not a pass and not a failure: there is no label check to run on this kind of
    # shard. Said plainly, because "0 checked" printed as a tick is how a gate
    # gets believed after it has stopped gating anything.
    Write-Warn2 ("$Gen carries game labels, which no fresh search reproduces - the bytes of " +
                 "$($samples.Count) unit(s) were checked and the labels were not")
    Write-Host  '         what gates a selfplay unit is regeneration from its seed, and the box'
    Write-Host  '         runs that against its own shard before it uploads one'
} elseif ($labelBad -eq 0) {
    Write-Ok "$labelChecked sampled labels from $($samples.Count) unit(s) reproduce exactly"
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
    if ($StrictVerify) { throw "label verification failed (-StrictVerify)" }
    Write-Warn2 'aggregating anyway; -StrictVerify -SkipDownload reruns this as a hard gate'
}

# The bucket files are a whole generation in size and they live for the length of
# the shuffle, so the directory is emptied on the way in as well as on the way
# out: a previous run that was killed left its own set here, and they are dead
# weight rather than anything to resume from.
if (Test-Path $TmpDir) { Get-ChildItem $TmpDir -File | Remove-Item -Force }
$TmpDir = Ensure-Dir $TmpDir

$outFile = Join-Path $DataDir "$Gen.cnn"
Write-Host "  shuffling $($shards.Count) shards -> $outFile"
& $Datagen shuffle -inputs $ListFile -o $outFile -seed $ShuffleSeed -tmp $TmpDir
if ($LASTEXITCODE -ne 0) { throw "shuffle failed" }
Get-ChildItem $TmpDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item $TmpDir -Force -ErrorAction SilentlyContinue

Write-Section "Result"
& $Datagen stats $outFile

Write-Host ""
Write-Host "  train with:"
Write-Host "    python -m nnue.train --train $outFile"
Write-Host "  record this generation in docs\EXPERIMENTS.md: size, nodes, eval, commit."
Write-Host ""
# What a Ctrl-C leaves behind, and what picks it up:
#   during the download - shards at their final names, some of them short. The
#     next run compares every local file against the hub's listing and refetches
#     the whole unit of anything that does not match, so this costs time and
#     nothing else. scp holds no lock across runs.
#   during the verify - nothing.
#   during the shuffle - the bucket files in <gen>\shuffle-tmp, up to the size of
#     the generation again, and a truncated <gen>.cnn/.pol. The next run empties
#     that directory before it starts; deleting it by hand is safe at any time
#     the shuffle is not running.
#   A truncated <gen>.cnn cannot be mistaken for a finished one: datagen removes
#     <gen>.json before it writes the first record and writes it again only after
#     the last, so an interrupted output has no manifest beside it. Nothing
#     downstream reads a shard whose manifest is missing without saying so.
