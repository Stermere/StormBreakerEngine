<#
.SYNOPSIS
    Runs the engine against a field of opponents and reports an Elo table.

.DESCRIPTION
    Where sprt.ps1 answers "is this patch better than the last version?", a
    gauntlet answers "how strong is the engine, actually?".

    Use it to:
      - confirm real progress across several baselines at once
      - catch regressions that self-play hides (an engine can beat its previous
        self while getting worse against different styles of opponent)
      - measure absolute strength against a known reference

    NOTE ON STOCKFISH: at full strength it will win 100% and tell you nothing.
    Use -SkillLevel to weaken it to a useful level, and raise it as you improve.

.EXAMPLE
    pwsh tools\gauntlet.ps1 -Games 200
    pwsh tools\gauntlet.ps1 -Opponents external\baselines\v0.1.exe
    pwsh tools\gauntlet.ps1 -IncludeStockfish -SkillLevel [0...20]
#>
[CmdletBinding()]
param(
    [string]  $Engine,
    [string[]]$Opponents   = @(),
    [int]     $Games       = 100,
    [string]  $Tc          = 'STC',
    [int]     $Concurrency = 0,
    [int]     $Hash        = 16,
    [switch]  $IncludeStockfish,
    [int]     $SkillLevel  = 0
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

$fastchess = Get-FastChess
if (-not $fastchess) {
    Write-Fail 'fastchess not found. Run: pwsh tools\setup.ps1'
    exit 1
}

if (-not $Engine) { $Engine = Get-EngineBinary }
if (-not $Engine) {
    Write-Fail "Engine not built. Run 'make' first."
    exit 1
}
$enginePath = (Resolve-Path $Engine).Path

$tcPresets = @{ 'VSTC' = '2+0.02'; 'STC' = '8+0.08'; 'LTC' = '40+0.4' }
if ($tcPresets.ContainsKey($Tc.ToUpper())) { $timeControl = $tcPresets[$Tc.ToUpper()] }
else { $timeControl = $Tc }

if ($Concurrency -le 0) { $Concurrency = Get-DefaultConcurrency }

# Default field: every saved baseline.
if ($Opponents.Count -eq 0) {
    $Opponents = @(Get-ChildItem (Ensure-Dir $BaselineDir) -Filter *.exe -ErrorAction SilentlyContinue |
                   ForEach-Object { $_.FullName })
}

$fcArgs = @('-engine', "cmd=$enginePath", 'name=engine')
$oppCount = 0

foreach ($opp in $Opponents) {
    if (-not (Test-Path $opp)) {
        Write-Warn2 "skipping missing opponent: $opp"
        continue
    }
    $oppPath = (Resolve-Path $opp).Path
    $oppName = [System.IO.Path]::GetFileNameWithoutExtension($oppPath)
    $fcArgs += @('-engine', "cmd=$oppPath", "name=$oppName")
    $oppCount++
}

if ($IncludeStockfish) {
    $sf = Get-Stockfish
    if ($sf) {
        $fcArgs += @('-engine', "cmd=$sf", "name=SF-skill$SkillLevel",
                     "option.Skill Level=$SkillLevel")
        $oppCount++
    } else {
        Write-Warn2 'Stockfish not found; continuing without it.'
    }
}

if ($oppCount -lt 1) {
    Write-Fail 'No opponents. Snapshot a baseline or pass -Opponents / -IncludeStockfish.'
    Write-Host '  pwsh tools\snapshot-baseline.ps1 -Name v0.1' -ForegroundColor Yellow
    exit 1
}

$book = Get-Book
$bookArgs = @()
if ($book) {
    $bookArgs = @('-openings', "file=$book", 'format=epd', 'order=random')
}

Ensure-Dir $GamesDir | Out-Null
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$pgnPath = Join-Path $GamesDir "$stamp-gauntlet.pgn"

$rounds = [math]::Max(1, [int]($Games / 2))

$fcArgs += @(
    '-each', "tc=$timeControl", "option.Hash=$Hash", 'option.Threads=1'
    '-rounds', "$rounds"
    '-games', '2'
    '-repeat'
    '-concurrency', "$Concurrency"
    '-pgnout', "file=$pgnPath"
    '-recover'
    # Round-robin so the opponents also play each other, which anchors the Elo
    # table instead of leaving every rating relative to a single engine.
    '-tournament', 'roundrobin'
) + $bookArgs

Write-Section 'Gauntlet'
Write-Host ("  engine       {0}" -f $enginePath)
Write-Host ("  opponents    {0}" -f $oppCount)
Write-Host ("  time control {0}" -f $timeControl)
Write-Host ("  games        ~{0} per pairing" -f $Games)
Write-Host ("  concurrency  {0}" -f $Concurrency)
Write-Host ("  pgn          {0}" -f $pgnPath)
Write-Host ''

& $fastchess @fcArgs
exit $LASTEXITCODE
