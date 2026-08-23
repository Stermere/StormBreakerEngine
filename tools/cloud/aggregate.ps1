# aggregate.ps1 - pull a generation off the hub and turn it into one shuffled
# training file.
#
#   .\tools\cloud\aggregate.ps1                  download, check, merge, shuffle
#   .\tools\cloud\aggregate.ps1 -Parallel 4      fewer concurrent scp streams
#   .\tools\cloud\aggregate.ps1 -SkipDownload    re-merge what is already local
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

if (-not (Test-Path $Datagen)) { throw "no datagen.exe at $Datagen - run: make datagen" }

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
# drifted from the rest of the fleet. -relabel needs the -nodes the pass used,
# not its own default.
$sample = (Get-ChildItem $MergeDir -Filter '*.cnn' | Select-Object -First 1).FullName
Write-Host "  verifying labels reproduce (relabel 256 @ $Nodes nodes)..."
& $Datagen verify $sample -relabel 256 -nodes $Nodes
if ($LASTEXITCODE -ne 0) { throw "label verification failed on $sample" }

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
