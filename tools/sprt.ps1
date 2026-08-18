<#
.SYNOPSIS
    Runs an SPRT match between a development build and a baseline.

.DESCRIPTION
    SPRT (Sequential Probability Ratio Test) plays games until it can accept or
    reject a hypothesis about the Elo difference, then stops. That is what makes
    it practical: a clearly good or clearly bad patch is resolved in a few
    hundred games, while only genuinely marginal ones cost tens of thousands.

    THE RULE: no patch that claims an Elo gain gets committed without passing
    this. Human intuition about chess engine changes is unreliable to the point
    of being actively misleading - roughly half of "obviously better" patches
    measure as neutral or worse.

.PARAMETER Dev
    Engine under test. Defaults to the freshly built .\chessengine.exe.

.PARAMETER Base
    Baseline to compare against. Defaults to the most recent snapshot in
    external\baselines (see snapshot-baseline.ps1).

.PARAMETER Tc
    Time control preset: STC (8+0.08), LTC (40+0.4), VSTC (2+0.02),
    or an explicit fastchess time control such as "10+0.1".

.PARAMETER Bounds
    SPRT bounds preset matching the Tc, or explicit "elo0,elo1".

.PARAMETER Smoke
    Ignores Dev/Base and plays Stockfish against itself for a handful of games.
    Verifies the whole pipeline - runner, book, concurrency, PGN output -
    without needing the engine to be able to play.

.EXAMPLE
    pwsh tools\sprt.ps1 -Smoke
    pwsh tools\sprt.ps1
    pwsh tools\sprt.ps1 -Tc LTC
    pwsh tools\sprt.ps1 -Dev .\chessengine.exe -Base external\baselines\v0.1.exe
#>
[CmdletBinding()]
param(
    [string]$Dev,
    [string]$Base,
    [string]$Tc          = 'STC',
    [string]$Bounds      = '',
    [int]   $Concurrency = 0,
    [int]   $Hash        = 16,
    [int]   $Threads     = 1,
    [int]   $Rounds      = 40000,
    [string]$Book        = '',
    [switch]$Smoke
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

$fastchess = Get-FastChess
if (-not $fastchess) {
    Write-Fail 'fastchess not found. Run: pwsh tools\setup.ps1'
    exit 1
}

# ------------------------------------------------------- time control -------

# STC finds most regressions cheaply; LTC catches search changes whose benefit
# only appears at depth (and is where a patch that "only helps at long TC" must
# prove itself). Always confirm an STC pass at LTC before committing.
$tcPresets = @{
    'VSTC' = '2+0.02'
    'STC'  = '8+0.08'
    'LTC'  = '40+0.4'
}

if ($tcPresets.ContainsKey($Tc.ToUpper())) {
    $timeControl = $tcPresets[$Tc.ToUpper()]
    $tcLabel     = $Tc.ToUpper()
} else {
    $timeControl = $Tc
    $tcLabel     = $Tc
}

# ------------------------------------------------------------ bounds --------

# elo0 is the null hypothesis (no gain), elo1 the alternative (a gain worth
# keeping). Wider bounds resolve faster but wave through smaller regressions.
$boundPresets = @{
    'VSTC' = @(0.0, 5.0)
    'STC'  = @(0.0, 5.0)
    'LTC'  = @(0.5, 4.5)
}

if ($Bounds) {
    $parts = $Bounds -split ','
    $elo0  = [double]$parts[0]
    $elo1  = [double]$parts[1]
} elseif ($boundPresets.ContainsKey($tcLabel)) {
    $elo0 = $boundPresets[$tcLabel][0]
    $elo1 = $boundPresets[$tcLabel][1]
} else {
    $elo0 = 0.0
    $elo1 = 5.0
}

if ($Concurrency -le 0) { $Concurrency = Get-DefaultConcurrency }

# ---------------------------------------------------------- engines ---------

if ($Smoke) {
    $sf = Get-Stockfish
    if (-not $sf) {
        Write-Fail 'Stockfish not found. Run: pwsh tools\setup.ps1'
        exit 1
    }
    $devPath = $sf
    $basePath = $sf
    $devName  = 'SF-dev'
    $baseName = 'SF-base'
    $timeControl = $tcPresets['VSTC']
    $Rounds = 2
} else {
    if (-not $Dev) { $Dev = Get-EngineBinary }
    if (-not $Dev) {
        Write-Fail "Engine not built. Run 'make' first."
        exit 1
    }
    $devPath = (Resolve-Path $Dev).Path

    if (-not $Base) {
        $snapshots = @(Get-ChildItem (Ensure-Dir $BaselineDir) -Filter *.exe -ErrorAction SilentlyContinue |
                       Sort-Object LastWriteTime -Descending)
        if ($snapshots.Count -gt 0) { $Base = $snapshots[0].FullName }
    }
    if (-not $Base) {
        Write-Fail 'No baseline found.'
        Write-Host  '  Snapshot the current build as the baseline first:' -ForegroundColor Yellow
        Write-Host  '    pwsh tools\snapshot-baseline.ps1 -Name v0.1' -ForegroundColor Yellow
        Write-Host  '  then make your change, rebuild, and re-run this script.' -ForegroundColor Yellow
        exit 1
    }
    $basePath = (Resolve-Path $Base).Path
    $devName  = 'dev'
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($basePath)
}

# ------------------------------------------------------------- book ---------

if (-not $Book) { $Book = Get-Book }
$bookArgs = @()
if ($Book -and (Test-Path $Book)) {
    # order=random with a per-run seed, and -repeat so both engines get each
    # opening from both colours. Without -repeat, colour luck becomes noise
    # that the SPRT has to spend extra games averaging away.
    $bookArgs = @('-openings', "file=$((Resolve-Path $Book).Path)", 'format=epd', 'order=random')
} else {
    Write-Warn2 'No opening book found - games will start from the initial position.'
    Write-Warn2 'This produces many duplicate games and unreliable results. Run tools\setup.ps1.'
}

# -------------------------------------------------------------- run ---------

Ensure-Dir $GamesDir | Out-Null
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$pgnPath = Join-Path $GamesDir "$stamp-$tcLabel.pgn"

$fcArgs = @(
    '-engine', "cmd=$devPath",  "name=$devName"
    '-engine', "cmd=$basePath", "name=$baseName"
    '-each', "tc=$timeControl", "option.Hash=$Hash", "option.Threads=$Threads"
    '-rounds', "$Rounds"
    '-games', '2'
    '-repeat'
    '-concurrency', "$Concurrency"
    '-pgnout', "file=$pgnPath"
    # Restart an engine that crashes instead of aborting the whole match. A
    # young engine WILL crash; losing 20000 games to it is avoidable.
    '-recover'
) + $bookArgs

if (-not $Smoke) {
    # model=normalized measures in normalised Elo, which makes results
    # comparable across time controls and books.
    $fcArgs += @('-sprt', "elo0=$elo0", "elo1=$elo1", 'alpha=0.05', 'beta=0.05', 'model=normalized')
}

Write-Section 'SPRT configuration'
Write-Host ("  dev          {0}" -f $devPath)
Write-Host ("  base         {0}" -f $basePath)
Write-Host ("  time control {0}  ({1})" -f $timeControl, $tcLabel)
if (-not $Smoke) {
    Write-Host ("  bounds       [{0}, {1}]  alpha=0.05 beta=0.05" -f $elo0, $elo1)
}
Write-Host ("  concurrency  {0}" -f $Concurrency)
Write-Host ("  hash/threads {0} MB / {1}" -f $Hash, $Threads)
if ($Book) { Write-Host ("  book         {0}" -f $Book) }
Write-Host ("  pgn          {0}" -f $pgnPath)
Write-Host ''

& $fastchess @fcArgs
$code = $LASTEXITCODE

Write-Host ''
if ($Smoke) {
    if ($code -eq 0) {
        Write-Ok 'Smoke test passed - runner, book, concurrency and PGN output all work.'
        if (Test-Path $pgnPath) {
            $games = ([regex]::Matches((Get-Content $pgnPath -Raw), '\[Event ')).Count
            Write-Ok "$games games written to $pgnPath"
        }
    } else {
        Write-Fail "Smoke test failed (exit $code)."
    }
}

exit $code
