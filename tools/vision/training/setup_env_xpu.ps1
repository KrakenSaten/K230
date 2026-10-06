# Create the Intel GPU (XPU) training environment on Windows: Python 3.10,
# upstream PyTorch XPU wheels at the PINS.env versions, requirements-train.txt
# and the YOLOX source at the pinned commit. Then runs xpu_check.py.
#
# Usage: setup_env_xpu.ps1 -Work C:\path\to\workdir
#   WORKDIR\venv-xpu   the environment
#   WORKDIR\src\YOLOX  YOLOX at YOLOX_COMMIT (use it as PYTHONPATH)
#
# Needs: the Intel Arc driver (it brings the Level Zero loader), git and uv.
# Use an ASCII-only WORKDIR path. Conversion (ONNX -> kmodel) stays on Linux:
# setup_env.sh train-cpu and convert, in WSL2.
param([Parameter(Mandatory = $true)][string]$Work)
$ErrorActionPreference = 'Stop'

$pins = @{}
foreach ($line in Get-Content (Join-Path $PSScriptRoot 'PINS.env')) {
    if ($line -match '^([A-Z_]+)="([^"$]*)"$') { $pins[$Matches[1]] = $Matches[2] }
}
New-Item -ItemType Directory -Force $Work | Out-Null
$venv = Join-Path $Work 'venv-xpu'
$py = Join-Path $venv 'Scripts\python.exe'
$yolox = Join-Path $Work 'src\YOLOX'

function Run { & $args[0] $args[1..($args.Count - 1)]; if ($LASTEXITCODE) { throw "failed: $args" } }

Run uv venv --python $pins.PYTHON_VERSION $venv
Run uv pip install --python $py "torch==$($pins.TORCH_XPU_VERSION)+xpu" "torchvision==$($pins.TORCHVISION_XPU_VERSION)+xpu" `
    --index-url https://download.pytorch.org/whl/xpu
Run uv pip install --python $py -r (Join-Path $PSScriptRoot 'requirements-train.txt')

if (-not (Test-Path (Join-Path $yolox '.git'))) { Run git clone -q $pins.YOLOX_REPO $yolox }
Run git -C $yolox checkout -q $pins.YOLOX_COMMIT
if ((git -C $yolox rev-parse HEAD) -ne $pins.YOLOX_COMMIT) { throw "YOLOX is not at $($pins.YOLOX_COMMIT)" }
Write-Output "YOLOX at $($pins.YOLOX_COMMIT)"

Run $py (Join-Path $PSScriptRoot 'xpu_check.py')
