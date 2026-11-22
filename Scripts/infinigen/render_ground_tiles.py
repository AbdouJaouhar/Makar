"""
Renders tileable ground textures from Infinigen's procedural terrain materials, for the island's terrain material
(Shaders/Ocean.ush, OceanTerrainSurface): a square of ground seen from above, its displaced geometry and its shader
rendered into albedo, normal and height maps, then made seamless.

Kinds (all found on Caribbean islands):
  limestone  Infinigen's Stone: the grey coral limestone of the rocky shores and cliffs;
  soil       Infinigen's Soil: the red-brown earth of the open hillsides, with pebbles;
  litter     Infinigen's Dirt strewn with fallen leaves and twigs (GroundLeaves, GroundTwigs): the forest floor;
  grass      Infinigen's Grass over Soil: the open, grassy ground of the clearings and hillsides.

The displacement and the shading come from 3D noise, which does not wrap, so the rendered square is made seamless
afterwards: blended with a copy of itself shifted by half a tile (whose wrap is continuous), each weighted by its
distance to its own seams.

Run inside the Infinigen environment in WSL:
    python render_ground_tiles.py <kind> <output folder> [resolution] [tile size m] [seed]
"""

import os
import sys

import bpy  # first: with bpy installed as a Python module, mathutils only exists once bpy is loaded
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_frond_atlas import render_top_view  # noqa: E402


def make_ground(kind, size, seed):
    """A finely subdivided square of ground with the kind's material (displacement + shader) and scatters."""
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=600, y_subdivisions=600, size=size * 1.2)  # margin for the scatters
    ground = bpy.context.active_object
    ground.name = f"Ground_{kind}"
    if kind == "limestone":
        from infinigen.assets.materials.terrain.stone import Stone
        Stone().apply(ground)
    elif kind == "soil":
        from infinigen.assets.materials.terrain.soil import Soil
        Soil().apply(ground)
    elif kind == "grass":
        from infinigen.assets.materials.terrain.soil import Soil
        from infinigen.assets.scatters.grass import Grass
        Soil().apply(ground)
        Grass().apply(ground)
    elif kind == "litter":
        from infinigen.assets.materials.terrain.dirt import Dirt
        from infinigen.assets.scatters.ground_leaves import GroundLeaves
        from infinigen.assets.scatters.ground_twigs import GroundTwigs
        Dirt().apply(ground)
        GroundLeaves().apply(ground, density=160, season="summer")
        GroundTwigs().apply(ground)
    else:
        raise ValueError(kind)
    return ground


def make_seamless(path):
    """Blends the image with its copy shifted by half a tile: each weighted by its distance to its own seams (the
    image's borders for the original, the central cross for the shifted copy, whose wrap is continuous)."""
    image = bpy.data.images.load(path)
    width, height = image.size
    channels = image.channels
    pixels = np.empty(width * height * channels, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    tile = pixels.reshape(height, width, channels)
    shifted = np.roll(tile, (height // 2, width // 2), axis=(0, 1))
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    to_border = np.minimum(np.minimum(x, width - 1 - x), np.minimum(y, height - 1 - y))
    to_cross = np.minimum(np.abs(x - width / 2.0), np.abs(y - height / 2.0))
    weight = (to_border / (to_border + to_cross + 1e-3))[..., None]
    blended = tile * weight + shifted * (1.0 - weight)
    if path.endswith("_normal.png"):
        # Renormalise the blended normals (stored 0-1).
        vector = blended[..., :3] * 2.0 - 1.0
        vector /= np.maximum(np.linalg.norm(vector, axis=-1, keepdims=True), 1e-6)
        blended[..., :3] = vector * 0.5 + 0.5
    image.pixels.foreach_set(blended.ravel())
    image.filepath_raw = path
    image.save()


def main(kind, out_dir, resolution, size, seed):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    make_ground(kind, size, seed)
    scene = bpy.context.scene
    scene.world = bpy.data.worlds.new("World")
    half = size / 2.0
    low, high = np.array([-half, -half, -1.0]), np.array([half, half, 1.0])
    render_top_view(out_dir, kind, resolution, low, high, margin=1.0, height=True)
    for suffix in ("basecolor", "normal", "height"):
        make_seamless(os.path.join(out_dir, f"{kind}_{suffix}.png"))
    print(f"render_ground_tiles: {kind}, {size:.1f} m tile -> {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2048,
         float(sys.argv[4]) if len(sys.argv) > 4 else 4.0, int(sys.argv[5]) if len(sys.argv) > 5 else 0)
