#!/usr/bin/env bash
# Generates the Caribbean asset set with Infinigen (in WSL2, as root), then decimates every FBX to a triangle budget
# for its category. Only things that belong in a Caribbean landscape: limestone boulders (recoloured on import),
# Caribbean reef corals (brain, elkhorn, star, sea fans), queen conch, urchins. Coconut palms are built separately
# (Infinigen renders fronds for leaf cards: its full palm geometry exceeds 260 M vertices once instances are real).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GEN=/root/makar/generate_asset.sh

declare -A BUDGET=( [rocks]=150000 [corals]=120000 [shells]=40000 )

run() { # category count factory
    "$GEN" "$1" "$2" "$3" | head -n 3
}

run rocks 4 BoulderFactory
run corals 2 BrainCoralFactory
run corals 2 ElkhornCoralFactory
run corals 2 StarCoralFactory
run corals 2 FanCoralFactory
run shells 2 ConchFactory
run shells 2 UrchinFactory

source /opt/miniforge/etc/profile.d/conda.sh
conda activate infinigen
for category in rocks corals shells; do
    for fbx in /root/makar/generated/$category/*/export_*/*.fbx; do
        [ -f "$fbx" ] || continue
        python "$HERE/decimate_fbx.py" "$fbx" "${BUDGET[$category]}" 2>&1 | grep decimate_fbx
    done
done
echo "SET DONE"
