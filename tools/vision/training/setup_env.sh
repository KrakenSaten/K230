#!/bin/bash
# Create the pinned environments (Linux x86-64, Python 3.10).
#
# Usage: setup_env.sh train-gpu|train-cpu|convert WORKDIR
#   train-gpu  WORKDIR/venv-train: torch 2.1.2+cu121 + requirements-train.txt,
#              and the YOLOX source at the pinned commit (PINS.env)
#   train-cpu  the same with torch 2.1.2+cpu (smoke runs)
#   convert    WORKDIR/venv-convert: nncase 2.11.0 + .NET 7 runtime in-tree
#
# YOLOX is used from its source tree (PYTHONPATH), not pip-installed, so
# the exact commit is what runs; run_record.py refuses a dirty or other tree.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/PINS.env"
WHAT=$1; W=$(realpath -m "$2"); mkdir -p "$W"

yolox() {
    if [ ! -d "$YOLOX_DIR/.git" ]; then
        git clone -q "$YOLOX_REPO" "$YOLOX_DIR"
    fi
    git -C "$YOLOX_DIR" fetch -q origin "$YOLOX_COMMIT" 2>/dev/null || true
    git -C "$YOLOX_DIR" checkout -q "$YOLOX_COMMIT"
    test "$(git -C "$YOLOX_DIR" rev-parse HEAD)" = "$YOLOX_COMMIT"
    echo "YOLOX at $YOLOX_COMMIT"
}

case "$WHAT" in
train-gpu|train-cpu)
    flavour=${WHAT#train-}; [ "$flavour" = gpu ] && flavour=$CUDA_WHEEL
    python$PYTHON_VERSION -m venv "$W/venv-train"
    "$W/venv-train/bin/pip" install -q --upgrade pip
    "$W/venv-train/bin/pip" install -q "torch==$TORCH_VERSION+$flavour" "torchvision==$TORCHVISION_VERSION+$flavour" \
        --index-url "https://download.pytorch.org/whl/$flavour"
    "$W/venv-train/bin/pip" install -q -r "$HERE/requirements-train.txt"
    yolox
    ;;
convert)
    python$PYTHON_VERSION -m venv "$W/venv-convert"
    "$W/venv-convert/bin/pip" install -q --upgrade pip
    "$W/venv-convert/bin/pip" install -q -r "$HERE/requirements-convert.txt"
    if [ ! -x "$W/dotnet/dotnet" ]; then
        curl -sSfL -o "$W/dotnet-install.sh" https://dot.net/v1/dotnet-install.sh
        bash "$W/dotnet-install.sh" --runtime dotnet --channel "$DOTNET_CHANNEL" --install-dir "$W/dotnet"
    fi
    # nncase finds the K230 plugin (nncase-kpu) through NNCASE_PLUGIN_PATH.
    SP=$("$W/venv-convert/bin/python" -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')
    cat > "$W/convert.env" <<EOF
export DOTNET_ROOT=$W/dotnet NNCASE_PLUGIN_PATH=$SP
export PATH=$W/dotnet:$W/venv-convert/bin:$SP:\$PATH
EOF
    echo "source $W/convert.env before compile_kmodel.py"
    ;;
*) echo "usage: $0 train-gpu|train-cpu|convert WORKDIR" >&2; exit 2 ;;
esac
