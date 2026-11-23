"""
Renders a seamless tile of beached sargassum (the wrack line at the top of Caribbean beaches) seen from above, for the
terrain shader (OceanTerrainSurface, Shaders/Ocean.ush): wrack_basecolor.png (RGBA: the mat, transparent where the sand
shows through), wrack_normal.png and wrack_height.png.

Infinigen's SeaweedFactory needs a compiled module that the minimal install leaves out, and it grows kelp, not
sargassum. So the sprigs are modelled here, after Sargassum fluitans / natans: a zigzag main stem with a few side
branches, small lance-shaped blades along them (2-4 cm), and clusters of round air bladders (3-5 mm) near the tips.
About seven hundred sprigs are heaped into a tangled mat with holes, fresh golden-orange ones over olive-brown and dried
near-black ones. Every sprig that crosses the tile's edge is copied on the opposite side, so the tile wraps exactly.

Run inside the Infinigen environment in WSL (Blender as a Python module):
    python render_wrack_tile.py <output folder> [resolution] [tile size m] [seed]
"""

import math
import os
import random
import sys

import bpy  # first: bmesh and mathutils only exist once bpy is loaded
import bmesh
import mathutils
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_frond_atlas import render_top_view  # noqa: E402


def sargassum_material():
    """Golden-orange fresh weed, olive-brown and dried near-black ones (one value per sprig); bladders a little lighter
    (vertex colour), slightly glossy when wet."""
    material = bpy.data.materials.new("Sargassum")
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    # A value per sprig from its pass index (not Object Info > Random, which hashes the name): the copies of a sprig
    # wrapped across the tile's edges keep its colour.
    golden = nodes.new("ShaderNodeMath")
    golden.operation = "MULTIPLY"
    links.new(nodes.new("ShaderNodeObjectInfo").outputs["Object Index"], golden.inputs[0])
    golden.inputs[1].default_value = 0.618034
    fraction = nodes.new("ShaderNodeMath")
    fraction.operation = "FRACT"
    links.new(golden.outputs[0], fraction.inputs[0])
    ramp_node = nodes.new("ShaderNodeValToRGB")
    links.new(fraction.outputs[0], ramp_node.inputs[0])
    ramp = ramp_node.color_ramp
    ramp.elements[0].position, ramp.elements[0].color = 0.0, (0.36, 0.2, 0.045, 1.0)   # fresh, golden-orange
    ramp.elements[1].position, ramp.elements[1].color = 0.45, (0.24, 0.13, 0.035, 1.0)  # olive-brown
    for position, colour in ((0.7, (0.12, 0.07, 0.03, 1.0)), (1.0, (0.045, 0.035, 0.028, 1.0))):  # drying, dried
        element = ramp.elements.new(position)
        element.color = colour
    lighter = nodes.new("ShaderNodeMixRGB")
    lighter.blend_type = "MULTIPLY"
    links.new(nodes.new("ShaderNodeVertexColor").outputs["Color"], lighter.inputs[0])
    links.new(ramp_node.outputs["Color"], lighter.inputs[1])
    lighter.inputs[2].default_value = (1.35, 1.3, 1.2, 1.0)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(lighter.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.45
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(bsdf.outputs[0], output.inputs["Surface"])
    return material


def add_blade(bm, base, direction, side, length, width, colour_layer, lighten):
    """A lance-shaped blade: base, two widest points at 40 %, tip; tilted a little out of the mat."""
    tip = base + direction * length
    middle = base + direction * (0.4 * length)
    corners = [base, middle + side * (width / 2.0), tip, middle - side * (width / 2.0)]
    verts = [bm.verts.new(corner) for corner in corners]
    face = bm.faces.new(verts)
    for loop in face.loops:
        loop[colour_layer] = (lighten, lighten, lighten, 1.0)


def add_bladder(bm, centre, radius, colour_layer):
    result = bmesh.ops.create_icosphere(bm, subdivisions=1, radius=radius, matrix=mathutils.Matrix.Translation(centre))
    faces = {f for v in result["verts"] for f in v.link_faces}
    for face in faces:
        for loop in face.loops:
            loop[colour_layer] = (1.0, 1.0, 1.0, 1.0)


def make_sprig(rng, length):
    """One sprig lying on the sand, centred on the origin: a zigzag stem with side branches, blades and bladders."""
    bm = bmesh.new()
    colour_layer = bm.loops.layers.color.new("Col")
    up = mathutils.Vector((0.0, 0.0, 1.0))

    def grow(start, heading, branch_length, depth):
        position = start.copy()
        segments = max(3, int(branch_length / 0.02))
        step = branch_length / segments
        for index in range(segments):
            heading = mathutils.Quaternion(up, rng.uniform(-0.5, 0.5)) @ heading  # zigzag
            heading.z = rng.uniform(-0.1, 0.15)
            heading.normalize()
            nxt = position + heading * step
            # The stem: a thin ribbon (1.5 mm) between the two points.
            side = heading.cross(up).normalized() * 0.0013
            verts = [bm.verts.new(p) for p in (position - side, nxt - side, nxt + side, position + side)]
            face = bm.faces.new(verts)
            for loop in face.loops:
                loop[colour_layer] = (0.8, 0.8, 0.8, 1.0)
            # A blade on alternate sides, lance-shaped, ~2-4 cm.
            direction = (mathutils.Quaternion(up, (1 if index % 2 else -1) * rng.uniform(0.5, 1.1)) @ heading)
            direction.z = rng.uniform(0.0, 0.35)
            direction.normalize()
            blade_side = direction.cross(up).normalized()
            add_blade(bm, position, direction, blade_side, rng.uniform(0.015, 0.03), rng.uniform(0.006, 0.01),
                      colour_layer, rng.uniform(0.85, 1.05))
            # Air bladders, in clusters towards the tips.
            if index > segments * 0.3 and rng.random() < 0.9:
                for _ in range(rng.randint(2, 4)):
                    offset = mathutils.Vector((rng.uniform(-0.008, 0.008), rng.uniform(-0.008, 0.008), rng.uniform(0.002, 0.006)))
                    add_bladder(bm, nxt + offset, rng.uniform(0.002, 0.003), colour_layer)
            # Side branches from the main stem.
            if depth == 0 and rng.random() < 0.3:
                branch = mathutils.Quaternion(up, rng.choice((-1, 1)) * rng.uniform(0.5, 1.0)) @ heading
                grow(position, branch, branch_length * rng.uniform(0.3, 0.55), 1)
            position = nxt

    heading = mathutils.Vector((1.0, 0.0, 0.0))
    grow(mathutils.Vector((-length / 2.0, 0.0, 0.0)), heading, length, 0)
    mesh = bpy.data.meshes.new("Sprig")
    bm.to_mesh(mesh)
    bm.free()
    return mesh


def main(out_dir, resolution, size, seed):
    rng = random.Random(seed)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    material = sargassum_material()
    sprigs = []
    for index in range(12):
        mesh = make_sprig(rng, rng.uniform(0.14, 0.3))
        mesh.materials.append(material)
        sprigs.append(mesh)

    # The mat: sprigs heaped in layers, thicker in patches and with holes (a coarse noise on the tile, wrapping).
    collection = bpy.context.scene.collection
    phase = [rng.uniform(0.0, 2.0 * math.pi) for _ in range(4)]

    def thickness(x, y):
        u, v = 2.0 * math.pi * x / size, 2.0 * math.pi * y / size
        return 0.5 + 0.25 * math.sin(u + phase[0]) * math.cos(v + phase[1]) + 0.25 * math.sin(2 * u + v + phase[2])

    placed = 0
    attempts = 0
    while placed < 700 and attempts < 6000:
        attempts += 1
        x, y = rng.uniform(0.0, size), rng.uniform(0.0, size)
        if rng.random() > thickness(x, y):
            continue
        layer = rng.uniform(0.0, 0.03) * thickness(x, y)
        rotation = rng.uniform(0.0, 2.0 * math.pi)
        mesh = rng.choice(sprigs)
        reach = 0.17  # half the longest sprig
        for dx in (-size, 0.0, size):
            for dy in (-size, 0.0, size):
                cx, cy = x + dx, y + dy
                if -reach < cx < size + reach and -reach < cy < size + reach:
                    obj = bpy.data.objects.new("Sprig", mesh)
                    obj.matrix_world = (mathutils.Matrix.Translation((cx, cy, layer))
                                        @ mathutils.Matrix.Rotation(rotation, 4, "Z"))
                    obj.pass_index = placed
                    collection.objects.link(obj)
        placed += 1
    print(f"render_wrack_tile: {placed} sprigs")

    bpy.context.scene.world = bpy.data.worlds.new("World")
    render_top_view(out_dir, "wrack", resolution, np.array([0.0, 0.0, -0.05]), np.array([size, size, 0.05]), margin=1.0,
                    height=True)
    print(f"render_wrack_tile: {size:.2f} m tile -> {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 2048, float(sys.argv[3]) if len(sys.argv) > 3 else 1.5,
         int(sys.argv[4]) if len(sys.argv) > 4 else 3)
