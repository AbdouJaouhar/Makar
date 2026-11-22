"""
Renders one frond of an Infinigen coconut palm, laid flat and seen from above, into textures for leaf cards.

The full Infinigen palm cannot go into a game (its realized geometry exceeds 260 M vertices), but its fronds are
the most realistic part of it. Game palms are drawn with cards: a few bent quads per frond, textured with a frond
image whose transparent background is cut out (alpha mask). This script:
  1. opens a generated palm (.blend), finds its frond mesh among the evaluated instances (the largest instanced
     mesh), and copies it into an empty scene;
  2. lays it flat: principal axes of its vertices -> length along Y, width along X, thinnest axis along Z;
  3. renders it from above with an orthographic camera, transparent film, Cycles passes:
       - diffuse colour (albedo) + alpha       -> frond_basecolor.png (RGBA)
       - normal (world space = card tangent space here, since the card is the XY plane) -> frond_normal.png
  4. writes frond.json with the frond's real length and width (m) for the card geometry.

Run inside the Infinigen environment in WSL:
    python render_frond_atlas.py <palm scene.blend> <output folder> [resolution]
"""

import json
import os
import sys

import bpy  # first: with bpy installed as a Python module, mathutils only exists once bpy is loaded
import mathutils
import numpy as np


def copy_frond(depsgraph, rank):
    """Copies the largest mesh that the palm instances several times: a whole frond (rachis + ~800 leaflets).
    Instance objects are temporary, valid only during the iteration: the copy is made inside the loop."""
    counts = {}
    for instance in depsgraph.object_instances:
        if not instance.is_instance or instance.object.type != "MESH" or instance.object.data is None:
            continue
        key = instance.object.data.as_pointer()
        entry = counts.setdefault(key, [len(instance.object.data.vertices), 0])
        entry[1] += 1
    candidates = {key: entry for key, entry in counts.items() if entry[1] >= 3}
    for vertices, uses in sorted(candidates.values(), key=lambda e: -e[0])[:6]:
        print(f"render_frond_atlas: candidate {vertices} vertices, used {uses} times")
    ranked = sorted(candidates.values(), key=lambda e: -e[0])
    target_vertices = ranked[rank][0]
    # Matched by vertex count (distinct here); the instance's own evaluated mesh is copied explicitly:
    # new_from_object() would re-evaluate the instance's original object, i.e. the whole tree.
    for instance in depsgraph.object_instances:
        if instance.is_instance and instance.object.type == "MESH" and instance.object.data is not None \
                and len(instance.object.data.vertices) == target_vertices:
            source = instance.object.data
            print(f"render_frond_atlas: copying {target_vertices} vertices, materials "
                  f"{[m.name for m in source.materials if m]}")
            return copy_mesh(source)
    raise RuntimeError("frond mesh not found again")


def copy_mesh(source):
    """A new mesh with the source's geometry, material slots and generic attributes (UVs, colours, the named
    attributes Infinigen's shaders read)."""
    mesh = bpy.data.meshes.new("Frond")
    vertices = np.empty(len(source.vertices) * 3, dtype=np.float32)
    source.vertices.foreach_get("co", vertices)
    loop_vertices = np.empty(len(source.loops), dtype=np.int32)
    source.loops.foreach_get("vertex_index", loop_vertices)
    loop_starts = np.empty(len(source.polygons), dtype=np.int32)
    source.polygons.foreach_get("loop_start", loop_starts)
    loop_totals = np.empty(len(source.polygons), dtype=np.int32)
    source.polygons.foreach_get("loop_total", loop_totals)
    material_indices = np.empty(len(source.polygons), dtype=np.int32)
    source.polygons.foreach_get("material_index", material_indices)

    mesh.vertices.add(len(source.vertices))
    mesh.vertices.foreach_set("co", vertices)
    mesh.loops.add(len(source.loops))
    mesh.loops.foreach_set("vertex_index", loop_vertices)
    mesh.polygons.add(len(source.polygons))
    mesh.polygons.foreach_set("loop_start", loop_starts)
    mesh.polygons.foreach_set("loop_total", loop_totals)
    mesh.polygons.foreach_set("material_index", material_indices)
    mesh.update(calc_edges=True)
    for material in source.materials:
        mesh.materials.append(material)

    sizes = {"FLOAT": 1, "INT": 1, "BOOLEAN": 1, "INT8": 1, "FLOAT_VECTOR": 3, "FLOAT2": 2, "FLOAT_COLOR": 4,
             "BYTE_COLOR": 4, "INT32_2D": 2, "QUATERNION": 4}
    fields = {"FLOAT": "value", "INT": "value", "BOOLEAN": "value", "INT8": "value", "FLOAT_VECTOR": "vector",
              "FLOAT2": "vector", "FLOAT_COLOR": "color", "BYTE_COLOR": "color", "INT32_2D": "value", "QUATERNION": "value"}
    dtypes = {"INT": np.int32, "BOOLEAN": bool, "INT8": np.int8, "INT32_2D": np.int32}
    for attribute in source.attributes:
        if attribute.name.startswith(".") or attribute.name in ("position", "material_index", "sharp_face") \
                or attribute.data_type not in sizes:
            continue
        domain_size = {"POINT": len(source.vertices), "CORNER": len(source.loops), "FACE": len(source.polygons),
                       "EDGE": len(source.edges)}.get(attribute.domain)
        if domain_size is None or attribute.domain == "EDGE":
            continue
        values = np.empty(domain_size * sizes[attribute.data_type], dtype=dtypes.get(attribute.data_type, np.float32))
        attribute.data.foreach_get(fields[attribute.data_type], values)
        target = mesh.attributes.get(attribute.name) or mesh.attributes.new(attribute.name, attribute.data_type, attribute.domain)
        target.data.foreach_set(fields[attribute.data_type], values)
    print(f"render_frond_atlas: copied attributes {[a.name for a in mesh.attributes if not a.name.startswith('.')]}")
    return mesh


def main(scene_path, out_dir, resolution, rank, source_name="Plane.001"):
    bpy.ops.wm.open_mainfile(filepath=scene_path)
    depsgraph = bpy.context.evaluated_depsgraph_get()
    # The palm assembles its crown from a source frond object (Plane.001 on Infinigen's coconut tree: 817 600
    # vertices, shader_leaf_material) instanced ~100 times: render that object itself. Reading meshes from the
    # depsgraph instances proved fragile (temporary objects, segfaults).
    if source_name:
        mesh = bpy.data.meshes.new_from_object(bpy.data.objects[source_name], preserve_all_data_layers=True, depsgraph=depsgraph)
    else:
        mesh = copy_frond(depsgraph, rank)

    # Empty scene with just the frond.
    for obj in list(bpy.context.scene.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    frond = bpy.data.objects.new("Frond", mesh)
    bpy.context.scene.collection.objects.link(frond)

    # Lay it flat: principal axes of the vertices (largest -> Y, middle -> X, smallest -> Z).
    points = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", points)
    points = points.reshape(-1, 3)
    center = points.mean(axis=0)
    _, _, axes = np.linalg.svd(points - center, full_matrices=False)
    length_axis, width_axis, thin_axis = axes[0], axes[1], axes[2]
    if np.dot(np.cross(width_axis, length_axis), thin_axis) < 0.0:
        thin_axis = -thin_axis
    rotation = np.stack([width_axis, length_axis, thin_axis])  # rows: new X, Y, Z
    flat = (points - center) @ rotation.T
    # Base of the frond (thick rachis end) at -Y: the end where the vertices are densest across X is the tip
    # otherwise, so flip so that the wider end is at +Y? Coconut fronds are widest in the middle; use the end
    # closest to the original mesh origin (the attachment point) as the base.
    origin_flat = (-center) @ rotation.T
    if origin_flat[1] > 0.0:
        flat[:, 1] = -flat[:, 1]
        flat[:, 0] = -flat[:, 0]
    mesh.vertices.foreach_set("co", flat.astype(np.float32).ravel())
    mesh.update()

    low, high = flat.min(axis=0), flat.max(axis=0)
    length = float(high[1] - low[1])
    width = float(high[0] - low[0])
    span = max(length, width) * 1.02
    mid = (low + high) * 0.5

    # Orthographic camera above, looking down -Z; the image covers span x span.
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = span
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = mathutils.Vector((float(mid[0]), float(mid[1]), float(high[2]) + 10.0))
    camera.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.scene.collection.objects.link(camera)
    scene = bpy.context.scene
    scene.camera = camera
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 32
    scene.render.film_transparent = True
    scene.render.resolution_x = resolution
    scene.render.resolution_y = resolution
    scene.render.resolution_percentage = 100
    scene.world = scene.world or bpy.data.worlds.new("World")
    layer = scene.view_layers[0]
    layer.use_pass_diffuse_color = True
    layer.use_pass_normal = True

    # Compositor: albedo with the render's alpha, and the normal pass remapped from [-1, 1] to [0, 1].
    scene.use_nodes = True
    tree = scene.node_tree
    tree.nodes.clear()
    layers = tree.nodes.new("CompositorNodeRLayers")
    set_alpha = tree.nodes.new("CompositorNodeSetAlpha")
    tree.links.new(layers.outputs["DiffCol"], set_alpha.inputs[0])
    tree.links.new(layers.outputs["Alpha"], set_alpha.inputs[1])
    normal_half = tree.nodes.new("CompositorNodeMixRGB")
    normal_half.blend_type = "MULTIPLY"
    normal_half.inputs[0].default_value = 1.0
    normal_half.inputs[2].default_value = (0.5, 0.5, 0.5, 1.0)
    tree.links.new(layers.outputs["Normal"], normal_half.inputs[1])
    normal_shift = tree.nodes.new("CompositorNodeMixRGB")
    normal_shift.blend_type = "ADD"
    normal_shift.inputs[0].default_value = 1.0
    normal_shift.inputs[2].default_value = (0.5, 0.5, 0.5, 1.0)
    tree.links.new(normal_half.outputs[0], normal_shift.inputs[1])
    normal_alpha = tree.nodes.new("CompositorNodeSetAlpha")
    tree.links.new(normal_shift.outputs[0], normal_alpha.inputs[0])
    tree.links.new(layers.outputs["Alpha"], normal_alpha.inputs[1])

    output = tree.nodes.new("CompositorNodeOutputFile")
    output.base_path = out_dir
    output.format.file_format = "PNG"
    output.format.color_mode = "RGBA"
    output.format.color_depth = "8"
    output.file_slots.clear()
    output.file_slots.new("frond_basecolor")
    output.file_slots.new("frond_normal")
    tree.links.new(set_alpha.outputs[0], output.inputs[0])
    tree.links.new(normal_alpha.outputs[0], output.inputs[1])
    composite = tree.nodes.new("CompositorNodeComposite")
    tree.links.new(set_alpha.outputs[0], composite.inputs[0])

    os.makedirs(out_dir, exist_ok=True)
    bpy.ops.render.render(write_still=False)
    # File Output appends the frame number: rename to stable names.
    for name in ("frond_basecolor", "frond_normal"):
        for file in os.listdir(out_dir):
            if file.startswith(name) and file != name + ".png":
                os.replace(os.path.join(out_dir, file), os.path.join(out_dir, name + ".png"))
    with open(os.path.join(out_dir, "frond.json"), "w") as f:
        json.dump({"length_m": length, "width_m": width, "image_span_m": span,
                   "center_offset_m": [float(mid[0] - low[0] - width * 0.5), float(mid[1])]}, f, indent=2)
    print(f"render_frond_atlas: frond {length:.2f} x {width:.2f} m, image span {span:.2f} m -> {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2048, int(sys.argv[4]) if len(sys.argv) > 4 else 1)
