# common.ps1 - shared paths and helpers, dot-sourced by the other tools.
#
# Everything is DISCOVERED rather than hardcoded, so the scripts keep working
# after a winget upgrade moves a binary, and on a different machine.
#
#   . "$PSScriptRoot\common.ps1"

Set-StrictMode -Version Latest

$RepoRoot    = Split-Path -Parent $PSScriptRoot
$ExternalDir = Join-Path $RepoRoot 'external'
$BooksDir    = Join-Path $ExternalDir 'books'
$EnginesDir  = Join-Path $ExternalDir 'engines'
$BaselineDir = Join-Path $ExternalDir 'baselines'
$GamesDir    = Join-Path $ExternalDir 'games'

function Ensure-Dir([string]$Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Force -Path $Path | Out-Null
    }
    return $Path
}

# Returns the first path that exists, or $null. Candidates may be command names
# (resolved via PATH) or absolute paths.
function Find-First([string[]]$Candidates) {
    foreach ($c in $Candidates) {
        if ([System.IO.Path]::IsPathRooted($c)) {
            if (Test-Path $c) { return (Resolve-Path $c).Path }
        } else {
            $cmd = Get-Command $c -ErrorAction SilentlyContinue
            if ($cmd) { return $cmd.Source }
        }
    }
    return $null
}

# fastchess: the match runner. winget installs it under the alias `fast-chess`.
function Get-FastChess {
    return Find-First @(
        'fastchess',
        'fast-chess',
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\fast-chess.exe"
    )
}

function Get-CuteChessCli {
    return Find-First @(
        'cutechess-cli',
        "$env:LOCALAPPDATA\Programs\Cute Chess\cutechess-cli.exe",
        "$env:ProgramFiles\Cute Chess\cutechess-cli.exe"
    )
}

function Get-CuteChessGui {
    return Find-First @(
        'cutechess',
        "$env:LOCALAPPDATA\Programs\Cute Chess\cutechess.exe",
        "$env:ProgramFiles\Cute Chess\cutechess.exe"
    )
}

function Get-EnCroissant {
    return Find-First @(
        "$env:LOCALAPPDATA\en-croissant\en-croissant.exe",
        "$env:LOCALAPPDATA\Programs\en-croissant\en-croissant.exe"
    )
}

function Get-Stockfish {
    return Find-First @(
        'stockfish',
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\stockfish.exe"
    )
}

# The freshly built engine.
function Get-EngineBinary {
    $exe = Join-Path $RepoRoot 'stormbreaker.exe'
    if (Test-Path $exe) { return $exe }
    return $null
}

# The opening book used for all testing.
function Get-Book {
    $book = Join-Path $BooksDir 'UHO_Lichess_4852_v1.epd'
    if (Test-Path $book) { return $book }
    return $null
}

# Writes text without a BOM. PowerShell 5.1's Set-Content/Out-File add one, and
# a BOM breaks JSON parsers - including Cute Chess reading engines.json.
function Write-TextNoBom([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
}

function Write-Section([string]$Title) {
    Write-Host ''
    Write-Host "== $Title" -ForegroundColor Cyan
}

function Write-Ok([string]$Message)   { Write-Host "  [ok]   $Message" -ForegroundColor Green }
function Write-Warn2([string]$Message) { Write-Host "  [warn] $Message" -ForegroundColor Yellow }
function Write-Fail([string]$Message) { Write-Host "  [fail] $Message" -ForegroundColor Red }

# Sensible concurrency for match running: leave two cores for the OS and for
# whatever else is running. Oversubscribing distorts every time-based result,
# which is the fastest way to get an SPRT verdict that does not reproduce.
function Get-DefaultConcurrency {
    $cores = [Environment]::ProcessorCount
    $c = $cores - 2
    if ($c -lt 1) { $c = 1 }
    return $c
}
