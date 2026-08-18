<#
.SYNOPSIS
    Freezes the current build as a testing baseline.

.DESCRIPTION
    Copies the built engine into external\baselines so later patches can be
    measured against it.

    WHY THIS MATTERS: Elo is only meaningful relative to something fixed. If
    you always test "new versus previous", small measurement errors compound in
    one direction and you can drift backwards while every individual test
    "passes". Keep a stable baseline, promote it deliberately, and periodically
    run a gauntlet against older baselines to confirm real progress.

.EXAMPLE
    pwsh tools\snapshot-baseline.ps1 -Name v0.1
    pwsh tools\snapshot-baseline.ps1            # names it from the git commit
#>
[CmdletBinding()]
param(
    [string]$Name,
    [string]$Source
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

if (-not $Source) { $Source = Get-EngineBinary }
if (-not $Source) {
    Write-Fail "Engine not built. Run 'make' first."
    exit 1
}

if (-not $Name) {
    # Fall back to a git-describe-ish label so snapshots stay traceable.
    $sha = ''
    try { $sha = (git rev-parse --short HEAD 2>$null) } catch { $sha = '' }
    if ($sha) { $Name = "base-$sha" }
    else      { $Name = "base-$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
}

Ensure-Dir $BaselineDir | Out-Null
$dest = Join-Path $BaselineDir "$Name.exe"

if (Test-Path $dest) {
    Write-Warn2 "Overwriting existing baseline $Name"
}

Copy-Item -Path $Source -Destination $dest -Force

# Record what this baseline actually is. Six months from now "base-a1b2c3d.exe"
# means nothing without it.
$benchLine = ''
try {
    $benchOut  = & $dest bench 2>$null
    $benchLine = ($benchOut | Select-String -Pattern '^\d+ nodes \d+ nps$' | Select-Object -First 1).ToString()
} catch { $benchLine = '(bench failed)' }

$meta = @(
    "name:    $Name",
    "source:  $Source",
    "date:    $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "commit:  $(try { git rev-parse HEAD 2>$null } catch { 'unknown' })",
    "bench:   $benchLine"
) -join "`n"

Write-TextNoBom (Join-Path $BaselineDir "$Name.txt") $meta

Write-Ok "Baseline saved: $dest"
Write-Host ''
Write-Host $meta
Write-Host ''
Write-Host 'Now make your change, rebuild, and run:' -ForegroundColor White
Write-Host "  pwsh tools\sprt.ps1 -Base `"$dest`""
