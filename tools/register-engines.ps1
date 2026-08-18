<#
.SYNOPSIS
    Registers the engine with Cute Chess (GUI and cutechess-cli).

.DESCRIPTION
    Adds (or updates) an entry in Cute Chess's engines.json so the engine shows
    up in the GUI's engine list and can be referenced by cutechess-cli as
    `-engine conf=<name>`.

    The registration points at the BUILT BINARY IN THIS REPO, so rebuilding
    picks up automatically - there is no need to re-register after every make.

    En Croissant stores its engine list in a database created on first launch,
    so it cannot be configured before the app has ever run. This script prints
    the exact steps instead; it is a four-click operation.

.EXAMPLE
    pwsh tools\register-engines.ps1
    pwsh tools\register-engines.ps1 -Name ChessEngine-dev
#>
[CmdletBinding()]
param(
    [string]$Name = 'ChessEngine',
    [string]$Engine
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

if (-not $Engine) { $Engine = Get-EngineBinary }
if (-not $Engine) {
    Write-Fail "Engine not built. Run 'make' first."
    exit 1
}
$enginePath = (Resolve-Path $Engine).Path

# ------------------------------------------------------------ Cute Chess ----

Write-Section 'Cute Chess'

# Cute Chess (Qt) keeps its settings under %APPDATA%\cutechess. Both layouts
# have been seen in the wild depending on version, so prefer whichever already
# exists and fall back to the flat one.
$candidates = @(
    (Join-Path $env:APPDATA 'cutechess'),
    (Join-Path $env:APPDATA 'cutechess\cutechess')
)

$configDir = $null
foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c 'engines.json')) { $configDir = $c; break }
}
if (-not $configDir) { $configDir = $candidates[0] }

Ensure-Dir $configDir | Out-Null
$enginesJson = Join-Path $configDir 'engines.json'

# Load the existing list so a hand-added engine is never clobbered.
$engines = @()
if (Test-Path $enginesJson) {
    try {
        $raw = Get-Content $enginesJson -Raw
        if ($raw.Trim()) {
            $parsed = $raw | ConvertFrom-Json
            if ($parsed -is [array]) { $engines = @($parsed) }
            else { $engines = @($parsed) }
        }
    } catch {
        Write-Warn2 "existing engines.json is not valid JSON; backing it up"
        Copy-Item $enginesJson "$enginesJson.bak" -Force
        $engines = @()
    }
}

# Drop any previous registration under this name, then re-add.
$engines = @($engines | Where-Object { $_.name -ne $Name })

$entry = [pscustomobject]@{
    name             = $Name
    command          = $enginePath
    workingDirectory = $RepoRoot
    protocol         = 'uci'
    options          = @()
}

$engines += $entry

# Cute Chess expects a JSON array. ConvertTo-Json unwraps single-element
# arrays, so force the shape when there is only one engine.
$json = ConvertTo-Json -InputObject @($engines) -Depth 6
if ($engines.Count -eq 1 -and -not $json.TrimStart().StartsWith('[')) {
    $json = "[$json]"
}

Write-TextNoBom $enginesJson $json
Write-Ok "registered '$Name' in $enginesJson"

# Verify by asking cutechess-cli to resolve the configuration by name. This is
# the real test: if the CLI can find it, so can the GUI.
$cli = Get-CuteChessCli
if ($cli) {
    $probe = & $cli -engine conf=$Name -engine conf=$Name -each tc=1+0 -rounds 1 -games 1 2>&1 | Out-String
    if ($probe -match 'Unknown engine configuration') {
        Write-Fail "cutechess-cli could not resolve '$Name'."
        Write-Warn2 "engines.json may belong at the other candidate path:"
        foreach ($c in $candidates) { Write-Host "    $c" }
    } else {
        Write-Ok "cutechess-cli resolves 'conf=$Name'"
    }
} else {
    Write-Warn2 'cutechess-cli not found; skipping verification.'
}

Write-Host ''
Write-Host '  Use it from the CLI with:' -ForegroundColor White
Write-Host "    cutechess-cli -engine conf=$Name -engine conf=$Name -each tc=10+0.1 -rounds 2"

# ---------------------------------------------------------- En Croissant ----

Write-Section 'En Croissant'

$ec = Get-EnCroissant
if (-not $ec) {
    Write-Warn2 'En Croissant not installed. Run: pwsh tools\setup.ps1'
} else {
    Write-Ok "installed at $ec"
    Write-Host ''
    Write-Host '  En Croissant builds its engine database on first launch, so it has to' -ForegroundColor Gray
    Write-Host '  be added through the UI once:' -ForegroundColor Gray
    Write-Host ''
    Write-Host '    1. Launch En Croissant'
    Write-Host '    2. Engines tab  ->  Add Engine  ->  Local'
    Write-Host "    3. Path:  $enginePath"
    Write-Host "    4. Name:  $Name"
    Write-Host ''
    Write-Host '  It points at the built binary, so rebuilds are picked up automatically.' -ForegroundColor Gray
}

Write-Host ''
Write-Ok 'Registration complete.'
