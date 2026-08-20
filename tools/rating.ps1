<#
.SYNOPSIS
    Estimates the engine's ABSOLUTE rating against a calibrated ladder.

.DESCRIPTION
    sprt.ps1 answers "is this better than the last version?" and gauntlet.ps1
    answers "how does it do against a field". Neither produces a number on a
    scale anyone outside this repository recognises, because every opponent in
    external\baselines is itself unrated.

    This plays the engine against Stockfish at several `UCI_Elo` settings and
    fits a single rating to the results by maximum likelihood.

    HOW MUCH TO TRUST THE NUMBER. Less than its confidence interval suggests.

      - `UCI_Elo` is Stockfish's own calibration against its own reference
        pool. It is approximate, and it is not the CCRL or FIDE scale even
        though it is expressed in the same units.
      - A strength-limited engine is not the same thing as a genuinely weaker
        one. Stockfish at UCI_Elo 2600 plays mostly 3600-strength moves with
        occasional deliberate errors; a real 2600 engine is uniformly weaker.
        Those two error distributions are not interchangeable, and an engine
        can score differently against them.
      - Ratings are time-control specific. A number measured at 8+0.08 is a
        blitz number and should not be compared to a 40/15 list.

    Treat the result as "roughly this class, on this ladder, at this time
    control" - good enough to decide whether a milestone has been reached, not
    good enough to quote as a rating.

.PARAMETER Levels
    UCI_Elo settings to play against. Pick a bracket around where you think
    the engine sits: levels it beats or loses to overwhelmingly carry almost
    no information, and the fit weights them accordingly.

.PARAMETER Games
    Games per level.

.EXAMPLE
    powershell tools\rating.ps1
    powershell tools\rating.ps1 -Levels 2600,2800,3000 -Games 200 -Tc LTC
#>
[CmdletBinding()]
param(
    [string] $Engine,
    # A comma-separated list rather than [int[]]: `powershell -File` passes
    # every argument as a string, and an [int[]] parameter fails to bind when
    # the script is launched that way (which is how it gets backgrounded).
    [string] $Levels      = '2200,2400,2600,2800,3000',
    [int]    $Games       = 100,
    [string] $Tc          = 'STC',
    [int]    $Concurrency = 0,
    [int]    $Hash        = 16
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

$fastchess = Get-FastChess
if (-not $fastchess) { Write-Fail 'fastchess not found. Run: pwsh tools\setup.ps1'; exit 1 }

$sf = Get-Stockfish
if (-not $sf) { Write-Fail 'Stockfish not found. Run: pwsh tools\setup.ps1'; exit 1 }

if (-not $Engine) { $Engine = Get-EngineBinary }
if (-not $Engine) { Write-Fail "Engine not built. Run 'make' first."; exit 1 }
$enginePath = (Resolve-Path $Engine).Path

$tcPresets = @{ 'VSTC' = '2+0.02'; 'STC' = '8+0.08'; 'LTC' = '40+0.4' }
if ($tcPresets.ContainsKey($Tc.ToUpper())) { $timeControl = $tcPresets[$Tc.ToUpper()] }
else { $timeControl = $Tc }

if ($Concurrency -le 0) { $Concurrency = Get-DefaultConcurrency }

$levelList = @($Levels -split '[,;\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ } |
               Sort-Object)
if ($levelList.Count -eq 0) { Write-Fail 'No valid -Levels given.'; exit 1 }

# ------------------------------------------------------------------ run -----

$fcArgs = @('-engine', "cmd=$enginePath", 'name=engine')
foreach ($lvl in $levelList) {
    # UCI_Elo is ignored unless UCI_LimitStrength is on - setting one without
    # the other silently gives you a full-strength Stockfish.
    $fcArgs += @('-engine', "cmd=$sf", "name=SF-$lvl",
                 'option.UCI_LimitStrength=true', "option.UCI_Elo=$lvl")
}

$book     = Get-Book
$bookArgs = @()
if ($book) { $bookArgs = @('-openings', "file=$book", 'format=epd', 'order=random') }

Ensure-Dir $GamesDir | Out-Null
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$pgnPath = Join-Path $GamesDir "$stamp-rating.pgn"

$fcArgs += @(
    '-each', "tc=$timeControl", "option.Hash=$Hash", 'option.Threads=1'
    '-rounds', "$([math]::Max(1, [int]($Games / 2)))"
    '-games', '2'
    '-repeat'
    '-concurrency', "$Concurrency"
    '-pgnout', "file=$pgnPath"
    '-recover'
    # gauntlet, not roundrobin: the ladder rungs playing each other would cost
    # most of the games and tell us nothing about our engine.
    '-tournament', 'gauntlet'
) + $bookArgs

Write-Section 'Rating estimate'
Write-Host ("  engine       {0}" -f $enginePath)
Write-Host ("  ladder       Stockfish UCI_Elo {0}" -f ($levelList -join ', '))
Write-Host ("  time control {0}" -f $timeControl)
Write-Host ("  games        {0} per level ({1} total)" -f $Games, ($Games * $levelList.Count))
Write-Host ("  pgn          {0}" -f $pgnPath)
Write-Host ''

$output = & $fastchess @fcArgs 2>&1
$output | ForEach-Object { Write-Host $_ }

# ------------------------------------------------------------- estimate -----
#
# Each level gives an independent estimate of our rating:
#
#     R = level + 400 * log10(S / (1 - S))
#
# with standard error 400 / (ln10 * sqrt(S(1-S)N)). Levels we sweep or get
# swept by have S near 0 or 1, where that error explodes - which is the correct
# behaviour, since such a result genuinely does not locate us. Combining by
# inverse variance therefore weights the informative rungs automatically.

# fastchess prints a ranking table for a gauntlet rather than a "Results of X
# vs Y" block per pairing, and the table's Score column is against the whole
# field. Tally from the PGN instead: it is exact, and it does not depend on how
# the runner chooses to format its summary.

$results = @()
if (Test-Path $pgnPath) {
    $tally = @{}
    foreach ($lvl in $levelList) {
        $tally["SF-$lvl"] = [pscustomobject]@{ Level = $lvl; Wins = 0; Losses = 0; Draws = 0 }
    }

    $white = ''; $black = ''
    foreach ($line in [System.IO.File]::ReadLines($pgnPath)) {
        if ($line.StartsWith('[White "'))  { $white = $line.Substring(8).TrimEnd('"]') ; continue }
        if ($line.StartsWith('[Black "'))  { $black = $line.Substring(8).TrimEnd('"]') ; continue }
        if (-not $line.StartsWith('[Result "')) { continue }

        $res = $line.Substring(9).TrimEnd('"]')
        $opp = if ($white -eq 'engine') { $black } elseif ($black -eq 'engine') { $white } else { '' }
        if (-not $opp -or -not $tally.ContainsKey($opp)) { continue }

        $engineIsWhite = ($white -eq 'engine')
        switch ($res) {
            '1-0'     { if ($engineIsWhite) { $tally[$opp].Wins++ }   else { $tally[$opp].Losses++ } }
            '0-1'     { if ($engineIsWhite) { $tally[$opp].Losses++ } else { $tally[$opp].Wins++ } }
            '1/2-1/2' { $tally[$opp].Draws++ }
        }
    }

    foreach ($k in $tally.Keys) {
        $t = $tally[$k]
        $n = $t.Wins + $t.Losses + $t.Draws
        if ($n -gt 0) {
            $results += [pscustomobject]@{
                Level = $t.Level; Games = $n; Wins = $t.Wins; Losses = $t.Losses; Draws = $t.Draws
            }
        }
    }
}

if ($results.Count -eq 0) {
    Write-Warn2 'No games found to score; read the table above by hand.'
    exit 0
}

Write-Section 'Per-level estimates'
Write-Host '  level    games    W-L-D          score     implied rating'
Write-Host '  -----    -----    ---------      -----     --------------'

$wsum = 0.0; $vsum = 0.0
foreach ($r in $results | Sort-Object Level) {
    $pts = $r.Wins + 0.5 * $r.Draws
    $s   = $pts / $r.Games
    $wld = "{0}-{1}-{2}" -f $r.Wins, $r.Losses, $r.Draws

    if ($s -le 0.0 -or $s -ge 1.0) {
        $note = if ($s -le 0.0) { 'lost every game' } else { 'won every game' }
        Write-Host ("  {0,5}    {1,5}    {2,-9}      {3,5:P1}     -- ({4})" -f
                    $r.Level, $r.Games, $wld, $s, $note)
        continue
    }

    $est = $r.Level + 400.0 * [Math]::Log10($s / (1.0 - $s))
    $se  = 400.0 / ([Math]::Log(10) * [Math]::Sqrt($s * (1 - $s) * $r.Games))
    $w   = 1.0 / ($se * $se)

    $wsum += $w * $est
    $vsum += $w

    Write-Host ("  {0,5}    {1,5}    {2,-9}      {3,5:P1}     {4,6:N0} +/- {5:N0}" -f
                $r.Level, $r.Games, $wld, $s, $est, (1.96 * $se))
}

Write-Host ''
if ($vsum -gt 0) {
    $combined = $wsum / $vsum
    $ci       = 1.96 / [Math]::Sqrt($vsum)
    Write-Ok ("Combined estimate: {0:N0} +/- {1:N0} Elo  ({2}, Stockfish UCI_Elo ladder)" -f
              $combined, $ci, $timeControl)
    Write-Host ''
    Write-Host '  The interval is statistical only. It does not include the' -ForegroundColor Yellow
    Write-Host '  calibration error of the ladder itself, which is larger.'  -ForegroundColor Yellow
    Write-Host '  Sanity check the ladder is engaging at all: the score should' -ForegroundColor Yellow
    Write-Host '  fall monotonically as the level rises.'                      -ForegroundColor Yellow
} else {
    Write-Warn2 'Every level was a sweep in one direction; re-run with a bracket that fits.'
}
