"""
Renders a tuft of Infinigen grass seen from the side, into a texture for grass cards (AOceanIslandFlora: crossed
quads along the forest edge, the top of the beaches and the clearings), like render_frond_atlas.py renders the palm
frond: <out>/tuft_basecolor.png (RGBA, cut out by alpha), tuft_normal.png, tuft.json (width and height, m).

A few tufts of Infinigen's GrassTuftFactory are set side by side, stood up so that their height runs up the image
(+Y), and rendered from the front (the scene's top view, render_top_view).

Run inside the Infinigen environment in WSL:
    python render_grass_tuft.py <output folder> [resolution] [seed]
"""

import json
import math
import os
import sys

import bpy  # first: with bpy installed as a Python module, mathutils only exists once bpy is loaded
import mathutils
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_frond_atlas import render_top_view  # noqa: E402


def blade_material():
    """Infinigen applies its grass material in the grass scatter, not to the tuft: blades shaded here along their
    height (the tuft's local Z, ~0.35 m): dark at the base, green, then straw at the tips, as Caribbean grasses in
    the dry season; uneven from blade to blade (noise over the tuft)."""
    material = bpy.data.materials.new("GrassBlade")
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    coords = nodes.new("ShaderNodeTexCoord").outputs["Object"]
    xyz = nodes.new("ShaderNodeSeparateXYZ")
    links.new(coords, xyz.inputs[0])
    height = nodes.new("ShaderNodeMath")
    height.operation = "DIVIDE"
    links.new(xyz.outputs["Z"], height.inputs[0])
    height.inputs[1].default_value = 0.35
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 40.0
    links.new(coords, noise.inputs["Vector"])
    shift = nodes.new("ShaderNodeMath")
    shift.operation = "MULTIPLY_ADD"
    links.new(noise.outputs["Fac"], shift.inputs[0])
    shift.inputs[1].default_value = 0.35
    links.new(height.outputs[0], shift.inputs[2])
    ramp_node = nodes.new("ShaderNodeValToRGB")
    links.new(shift.outputs[0], ramp_node.inputs[0])
    ramp = ramp_node.color_ramp
    ramp.elements[0].position, ramp.elements[0].color = 0.05, (0.04, 0.06, 0.015, 1.0)
    ramp.elements[1].position, ramp.elements[1].color = 0.45, (0.11, 0.16, 0.035, 1.0)
    for position, colour in ((0.75, (0.17, 0.19, 0.05, 1.0)), (1.05, (0.36, 0.3, 0.13, 1.0))):
        element = ramp.elements.new(position)
        element.color = colour
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(ramp_node.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.6
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(bsdf.outputs[0], output.inputs["Surface"])
    return material


def main(out_dir, resolution, seed):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    from infinigen.assets.objects.grassland.grass_tuft import GrassTuftFactory
    factory = GrassTuftFactory(seed)
    material = blade_material()
    low = np.array([1e9, 1e9, 1e9])
    high = -low
    # Three tufts in a row, overlapping: one dense clump of blades. Stood up: their height (+Z) along +Y, seen from +Z.
    for index, offset in enumerate((-0.07, 0.0, 0.075)):
        tuft = factory.spawn_asset(index)
        if tuft.type == "MESH":
            tuft.data.materials.clear()
            tuft.data.materials.append(material)
        tuft.matrix_world = (mathutils.Matrix.Translation((offset, 0.0, 0.02 * index))
                             @ mathutils.Matrix.Rotation(-math.pi / 2.0, 4, "X")
                             @ mathutils.Matrix.Rotation(index * 1.3, 4, "Z"))
        bpy.context.view_layer.update()
        corners = np.array([tuft.matrix_world @ mathutils.Vector(corner) for corner in tuft.bound_box])
        low = np.minimum(low, corners.min(axis=0))
        high = np.maximum(high, corners.max(axis=0))
    scene = bpy.context.scene
    scene.world = bpy.data.worlds.new("World")
    span, mid = render_top_view(out_dir, "tuft", resolution, low, high)
    width, height = float(high[0] - low[0]), float(high[1] - low[1])
    with open(os.path.join(out_dir, "tuft.json"), "w") as f:
        json.dump({"width_m": width, "height_m": height, "image_span_m": span,
                   "base_v": float(0.5 + (mid[1] - low[1]) / span)}, f, indent=2)
    print(f"render_grass_tuft: tuft {width:.2f} x {height:.2f} m, image span {span:.2f} m -> {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1024, int(sys.argv[3]) if len(sys.argv) > 3 else 0)
