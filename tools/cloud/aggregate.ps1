# aggregate.ps1 - pull a generation off the hub and turn it into one shuffled
# training file.
#
#   .\tools\cloud\aggregate.ps1              download, check, merge, shuffle
#   .\tools\cloud\aggregate.ps1 -SkipDownload    re-merge what is already local
#
# The download is the slow part (~6.5 GB for a full human pass), so every
# integrity check that can run before the shuffle runs before the shuffle.

[CmdletBinding()]
param(
    [string]$ConfigPath,
    [switch]$SkipDownload,
    [int]$ShuffleSeed = 7
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

$Datagen  = Join-Path $RepoRoot 'datagen.exe'
$DataDir  = Ensure-Dir (Join-Path $ExternalDir 'data')
$GenDir   = Ensure-Dir (Join-Path $DataDir $Gen)
$MergeDir = Ensure-Dir (Join-Path $GenDir 'merged')

if (-not (Test-Path $Datagen)) { throw "no datagen.exe at $Datagen - run: make datagen" }

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

Write-Section "Aggregating $Gen"

if (-not $SkipDownload) {
    Write-Host "  downloading $HubDir/$Gen/shards -> $GenDir"
    & scp @IdentityArgs -P $HubPort -q -r "${HubUser}:$HubDir/$Gen/shards/*" $GenDir
    if ($LASTEXITCODE -ne 0) { throw "download failed" }
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
