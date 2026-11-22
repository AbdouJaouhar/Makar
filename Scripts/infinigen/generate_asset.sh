#!/usr/bin/env bash
# generate_asset.sh <category> <count> <Factory> [extra args]: one Infinigen run in WSL2 (as root), FBX export with
# baked textures + a CPU preview render (the WSL GPU driver logged dxg errors during Cycles GPU renders).
# Outputs: /root/makar/generated/<category>/<Factory>/ (previews, logs) and its export_NNN/ folders (FBX + textures).
set -uo pipefail
CATEGORY=$1; COUNT=$2; FACTORY=$3; shift 3
source /opt/miniforge/etc/profile.d/conda.sh
conda activate infinigen
cd /root/infinigen
OUT=/root/makar/generated/$CATEGORY/${FACTORY##*.}
mkdir -p "$OUT"
start=$(date +%s)
python -m infinigen_examples.generate_individual_assets -o "$OUT" -f "$FACTORY" -n "$COUNT" \
    -r image -p 32 --n_workers 1 --export fbx --export_texture_res 1024 "$@" > "$OUT/$FACTORY.log" 2>&1
code=$?
echo "$FACTORY: exit $code in $(( $(date +%s) - start )) s"
find "$OUT" -newer /root/makar/install_v1.log -type f \( -name '*.fbx' -o -name '*.png' -o -name '*.jpg' \) | head -40
tail -n 5 "$OUT/$FACTORY.log"
