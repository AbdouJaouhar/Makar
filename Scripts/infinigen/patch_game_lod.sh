#!/usr/bin/env bash
# Lowers the film-quality resolutions of some Infinigen generators to game-ready levels, in the local clone
# (/root/infinigen). Idempotent: each sed only matches the original value.
#
# CoconutTreeFactory came out at 19 M triangles (17 463 leaflet instances), and realizing its instances for
# export ran out of memory (16 GB). The geometry is dominated by:
#   - the trunk, subdivided 5 times (x1024 faces)          -> 2 times
#   - the cross-section of every leaflet, 128 points        -> 16
#   - the leaf vein curve, 400 points                       -> 60
set -euo pipefail
cd /root/infinigen/src/infinigen/assets/objects/tropic_plants

sed -i 's/Nodes.SubdivideMesh, input_kwargs={"Mesh": curve_to_mesh, "Level": 5}/Nodes.SubdivideMesh, input_kwargs={"Mesh": curve_to_mesh, "Level": 2}/' coconut_tree.py
sed -i 's/Nodes.CurveCircle, input_kwargs={"Resolution": 128, "Radius": 0.05}/Nodes.CurveCircle, input_kwargs={"Resolution": 16, "Radius": 0.05}/' coconut_tree.py
sed -i 's/"Resolution": 400,/"Resolution": 60,/' coconut_tree.py

grep -nE '"Level": [0-9]+|"Resolution": (16|60|128|400)' coconut_tree.py
echo "PATCHED"
