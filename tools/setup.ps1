<#
.SYNOPSIS
    Installs and verifies everything needed to test the engine.

.DESCRIPTION
    Idempotent: re-run it any time. It installs missing tools via winget,
    downloads the opening book, and prints a status table.

    Installed:
      fastchess      match runner and SPRT driver
      Cute Chess     GUI + cutechess-cli
      En Croissant   modern analysis GUI
      Stockfish      reference opponent and perft cross-check

    Downloaded:
      UHO_Lichess_4852_v1.epd  the standard SPRT opening book

.EXAMPLE
    powershell -File tools\setup.ps1
    powershell -File tools\setup.ps1 -SkipInstall     # book only, no winget
#>
[CmdletBinding()]
param(
    [switch]$SkipInstall,
    [switch]$Force
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

Write-Host 'Chess engine development environment setup' -ForegroundColor White

# --------------------------------------------------------------- tooling ----

if (-not $SkipInstall) {
    Write-Section 'Installing tools via winget'

    $packages = @(
        @{ Id = 'Disservin.FastChess';    Name = 'fastchess';    Probe = { Get-FastChess } },
        @{ Id = 'CuteChess.CuteChess';    Name = 'Cute Chess';   Probe = { Get-CuteChessCli } },
        @{ Id = 'EnCroissant.EnCroissant';Name = 'En Croissant'; Probe = { Get-EnCroissant } },
        @{ Id = 'Stockfish.Stockfish';    Name = 'Stockfish';    Probe = { Get-Stockfish } }
    )

    foreach ($pkg in $packages) {
        $existing = & $pkg.Probe
        if ($existing -and -not $Force) {
            Write-Ok "$($pkg.Name) already installed"
            continue
        }

        Write-Host "  installing $($pkg.Name) ($($pkg.Id)) ..."
        winget install --exact --id $pkg.Id `
            --accept-package-agreements --accept-source-agreements --disable-interactivity | Out-Null

        if (& $pkg.Probe) { Write-Ok "$($pkg.Name) installed" }
        else { Write-Warn2 "$($pkg.Name) installed but not found on PATH - open a new shell" }
    }
}

# ------------------------------------------------------------------ book ----

Write-Section 'Opening book'

Ensure-Dir $BooksDir | Out-Null
$bookPath = Join-Path $BooksDir 'UHO_Lichess_4852_v1.epd'

if ((Test-Path $bookPath) -and -not $Force) {
    Write-Ok "UHO_Lichess_4852_v1.epd already present"
} else {
    # UHO = "Unbalanced Human Openings". Positions are pre-scored to be slightly
    # unbalanced, which produces far fewer draws than a balanced book. Fewer
    # draws means each game carries more information, so an SPRT reaches its
    # verdict in dramatically fewer games. This is the community standard book.
    $url = 'https://github.com/official-stockfish/books/raw/master/UHO_Lichess_4852_v1.epd.zip'
    $zip = Join-Path $BooksDir 'UHO_Lichess_4852_v1.epd.zip'

    Write-Host "  downloading $url"
    $progressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

    Write-Host '  extracting ...'
    Expand-Archive -Path $zip -DestinationPath $BooksDir -Force
    Remove-Item $zip -Force

    if (Test-Path $bookPath) {
        $lines = (Get-Content $bookPath | Measure-Object -Line).Lines
        Write-Ok "UHO_Lichess_4852_v1.epd ($lines positions)"
    } else {
        Write-Fail 'book extraction did not produce the expected file'
    }
}

# ----------------------------------------------------------- directories ----

Write-Section 'Working directories'
foreach ($d in @($EnginesDir, $BaselineDir, $GamesDir)) {
    Ensure-Dir $d | Out-Null
    Write-Ok (Resolve-Path $d).Path.Replace($RepoRoot, '.')
}

# --------------------------------------------------------------- summary ----

Write-Section 'Status'

$rows = @(
    [pscustomobject]@{ Component = 'fastchess';    Path = Get-FastChess }
    [pscustomobject]@{ Component = 'cutechess-cli';Path = Get-CuteChessCli }
    [pscustomobject]@{ Component = 'Cute Chess UI';Path = Get-CuteChessGui }
    [pscustomobject]@{ Component = 'En Croissant'; Path = Get-EnCroissant }
    [pscustomobject]@{ Component = 'Stockfish';    Path = Get-Stockfish }
    [pscustomobject]@{ Component = 'opening book'; Path = Get-Book }
    [pscustomobject]@{ Component = 'engine';       Path = Get-EngineBinary }
)

foreach ($r in $rows) {
    if ($r.Path) { Write-Ok ("{0,-14} {1}" -f $r.Component, $r.Path) }
    else         { Write-Warn2 ("{0,-14} MISSING" -f $r.Component) }
}

if (-not (Get-EngineBinary)) {
    Write-Host ''
    Write-Host "  The engine is not built yet. Run 'make' in the repo root." -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'Next steps:' -ForegroundColor White
Write-Host '  make                          build the engine'
Write-Host '  powershell -File tools\register-engines.ps1   register it with Cute Chess'
Write-Host '  make sprt ARGS=--smoke            verify the match pipeline end to end'
