#!/usr/bin/env bash
# Installs Infinigen (princeton-vl/infinigen) for single-asset generation, in WSL2 Ubuntu 24.04, as root.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo "== apt"
apt-get update -q
apt-get install -y -q wget git cmake g++ libgles2-mesa-dev libglew-dev libglfw3-dev libglm-dev zlib1g-dev \
    libxi6 libxxf86vm1 libxfixes3 libxrender1 libgl1 libsm6 libxkbcommon0

echo "== miniforge"
if [ ! -d /opt/miniforge ]; then
    wget -q https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh -O /tmp/miniforge.sh
    bash /tmp/miniforge.sh -b -p /opt/miniforge
fi
source /opt/miniforge/etc/profile.d/conda.sh

echo "== infinigen"
if [ ! -d /root/infinigen ]; then
    git clone --depth 1 https://github.com/princeton-vl/infinigen.git /root/infinigen
fi
cd /root/infinigen
git log --oneline -1
if ! conda env list | grep -q '^infinigen '; then
    conda create -y -q --name infinigen python=3.11
fi
conda activate infinigen
# Minimal install: no terrain or OpenGL ground truth, enough for individual assets.
INFINIGEN_MINIMAL_INSTALL=True pip install -q -e .

echo "== check"
python -c "import bpy, infinigen; print('bpy', bpy.app.version_string, 'infinigen ok')"
echo "== asset factories"
ls infinigen/assets/objects
echo "INSTALL DONE"
