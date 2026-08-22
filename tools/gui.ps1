<#
.SYNOPSIS
    Launches a chess GUI with the current engine build registered.

.DESCRIPTION
    Re-registers the engine before launching, so the GUI always points at the
    binary you just built. Registration is by path, not by copy, so this is
    only strictly necessary the first time - but re-running it is free and
    removes a whole class of "why is the GUI running my old engine?" confusion.

    The GUI is started detached: your terminal stays usable.

.PARAMETER App
    cutechess    Cute Chess - engine matches, tournaments, debugging (default)
    encroissant  En Croissant - analysis, opening explorer, databases
    both         launch both

.PARAMETER NoRegister
    Skip re-registration and just launch.

.EXAMPLE
    pwsh tools\gui.ps1
    pwsh tools\gui.ps1 -App cutechess
    pwsh tools\gui.ps1 -App both
#>
[CmdletBinding()]
param(
    [ValidateSet('cutechess', 'encroissant', 'both')]
    [string]$App = 'encroissant',
    [switch]$NoRegister
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

$engine = Get-EngineBinary
if (-not $engine) {
    Write-Fail "Engine not built. Run 'make' first."
    exit 1
}

if (-not $NoRegister) {
    Write-Section 'Refreshing engine registration'
    & powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot 'register-engines.ps1') | Out-Null
    Write-Ok "engine registered: $engine"
}

$launched = 0

if ($App -eq 'cutechess' -or $App -eq 'both') {
    $exe = Get-CuteChessGui
    if ($exe) {
        Write-Section 'Cute Chess'
        Start-Process -FilePath $exe
        Write-Ok 'launched'
        Write-Host '  Play a game:  Game -> New  ->  set one side to CPU / ChessEngine' -ForegroundColor Gray
        Write-Host '  Tournament:   Tournament -> New' -ForegroundColor Gray
        $launched++
    } else {
        Write-Fail 'Cute Chess not installed. Run: pwsh tools\setup.ps1'
    }
}

if ($App -eq 'encroissant' -or $App -eq 'both') {
    $exe = Get-EnCroissant
    if ($exe) {
        Write-Section 'En Croissant'
        Start-Process -FilePath $exe
        Write-Ok 'launched'
        Write-Host '  First run only: Engines -> Add Engine -> Local' -ForegroundColor Gray
        Write-Host "                  path: $engine" -ForegroundColor Gray
        $launched++
    } else {
        Write-Fail 'En Croissant not installed. Run: pwsh tools\setup.ps1'
    }
}

if ($launched -eq 0) { exit 1 }
