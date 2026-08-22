<#
.SYNOPSIS
    Creates trainer/.venv and installs PyTorch into it.

.DESCRIPTION
    The trainer is Python and the engine is not: it gets its own virtualenv,
    it is not part of the C build, and nothing in src/ ever imports from it.

    Two things this script exists to get right, both of which are easy to get
    wrong by hand:

      * PyTorch wheels lag new CPython releases. If the newest Python on the
        machine has no torch wheel, this picks an older interpreter rather than
        failing - the engine does not care what Python the trainer runs on.

      * On Windows the `torch` wheel on PyPI is CPU-ONLY. A CUDA build has to
        come from download.pytorch.org, and installing the wrong one is a
        silent 50x slowdown rather than an error.

.PARAMETER CudaTag
    Which CUDA build to install: cu126 (default), cu128, cu130, or 'cpu' for
    the CPU-only wheel.

.PARAMETER Python
    An explicit interpreter to build the venv from. By default the newest
    version that has a torch wheel is chosen automatically.

.PARAMETER Force
    Delete an existing trainer/.venv and start again.

.EXAMPLE
    pwsh tools\trainer-setup.ps1
    pwsh tools\trainer-setup.ps1 -CudaTag cpu
#>
[CmdletBinding()]
param(
    [ValidateSet('cu126', 'cu128', 'cu130', 'cpu')]
    [string]$CudaTag = 'cu126',

    [string]$Python,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repo    = Split-Path -Parent $PSScriptRoot
$trainer = Join-Path $repo 'trainer'
$venv    = Join-Path $trainer '.venv'
$venvPython = Join-Path $venv 'Scripts\python.exe'

if (-not (Test-Path $trainer)) { throw "no trainer/ directory at $trainer" }

# ---------------------------------------------------------- pick a Python --
# Newest first; torch supports a range of CPython versions and the newest
# release is the one most likely to be missing a wheel.
# Windows PowerShell 5.1 turns a native command's redirected stderr into
# ErrorRecords and, under `$ErrorActionPreference = 'Stop'`, that aborts the
# script. `py` prints its "Installed Pythons" banner to stderr whenever the
# version asked for is missing, so probing for interpreters has to relax the
# preference and filter the records back out.
function Resolve-PythonVersion([string]$Tag) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & py "-$Tag" -c 'import sys; print(sys.executable)' 2>&1 |
            Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] }
        if ($LASTEXITCODE -eq 0 -and $output) { return "$output".Trim() }
    } catch {
        # `py` itself is not installed; the caller falls back to `python`.
    } finally {
        $ErrorActionPreference = $previous
    }
    return $null
}

function Resolve-PythonOnPath {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & python -c 'import sys; print(sys.executable)' 2>&1 |
            Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] }
        if ($LASTEXITCODE -eq 0 -and $output) { return "$output".Trim() }
    } catch {
    } finally {
        $ErrorActionPreference = $previous
    }
    return $null
}

function Find-Python {
    if ($Python) {
        if (-not (Test-Path $Python)) { throw "no interpreter at $Python" }
        return $Python
    }

    $candidates = @()
    foreach ($tag in '3.13', '3.12', '3.14', '3.11') {
        $found = Resolve-PythonVersion $tag
        if ($found) { $candidates += [pscustomobject]@{ Tag = $tag; Path = $found } }
    }
    if ($candidates.Count -eq 0) {
        $found = Resolve-PythonOnPath
        if ($found) { $candidates += [pscustomobject]@{ Tag = 'default'; Path = $found } }
    }
    if ($candidates.Count -eq 0) {
        throw 'no Python found. Install one from https://python.org, or run: winget install Python.Python.3.13'
    }

    Write-Host "found Python:" -ForegroundColor Cyan
    $candidates | ForEach-Object { Write-Host "  $($_.Tag)  $($_.Path)" }
    return $candidates[0].Path
}

$interpreter = Find-Python
Write-Host "using $interpreter" -ForegroundColor Green

# ------------------------------------------------------------ build the venv --
if ((Test-Path $venv) -and $Force) {
    Write-Host "removing the existing venv" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $venv
}

if (-not (Test-Path $venvPython)) {
    Write-Host "creating $venv" -ForegroundColor Cyan
    & $interpreter -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "python -m venv failed" }
}

& $venvPython -m pip install --upgrade pip --quiet
if ($LASTEXITCODE -ne 0) { throw "pip upgrade failed" }

# ------------------------------------------------------------------ torch --
$index = if ($CudaTag -eq 'cpu') {
    'https://download.pytorch.org/whl/cpu'
} else {
    "https://download.pytorch.org/whl/$CudaTag"
}

$requirements = Join-Path $trainer 'requirements.txt'
$torchPin = (Select-String -Path $requirements -Pattern '^torch==' | Select-Object -First 1).Line
if (-not $torchPin) { $torchPin = 'torch' }

Write-Host "installing $torchPin from $index" -ForegroundColor Cyan
& $venvPython -m pip install $torchPin --index-url $index
if ($LASTEXITCODE -ne 0) {
    Write-Warning "the pinned torch build is not available for this interpreter and CUDA tag."
    Write-Warning "try a different -CudaTag (cu126 / cu128 / cu130 / cpu), or -Python pointing at 3.12 or 3.13."
    throw "torch install failed"
}

# Everything else comes from PyPI; only torch needs the CUDA index.
Write-Host "installing the rest of requirements.txt" -ForegroundColor Cyan
& $venvPython -m pip install -r $requirements
if ($LASTEXITCODE -ne 0) { throw "pip install -r requirements.txt failed" }

# ------------------------------------------------------------------ verify --
Write-Host ""
& $venvPython -c @'
import torch, numpy
print(f'torch  {torch.__version__}')
print(f'numpy  {numpy.__version__}')
if torch.cuda.is_available():
    print(f'cuda   {torch.version.cuda} on {torch.cuda.get_device_name(0)}')
else:
    print('cuda   NOT AVAILABLE - training will run on the CPU and be very slow.')
    print('       Re-run with a different -CudaTag if this machine has an NVIDIA GPU.')
'@
if ($LASTEXITCODE -ne 0) { throw 'torch does not import' }

Write-Host ""
Write-Host "ready. Next:" -ForegroundColor Green
Write-Host "  make datagen-test                        generate a small shard and verify it"
Write-Host "  make trainer-test                        run the trainer's test suite"
Write-Host "  trainer\.venv\Scripts\python.exe -m nnue.train --help"
Write-Host ""
Write-Host 'run the trainer from the trainer/ directory so that nnue is importable.' -ForegroundColor DarkGray
