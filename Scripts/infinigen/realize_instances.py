"""
Turns the geometry-node instances of an Infinigen asset into one real mesh, without a Realize Instances node.

Infinigen's exporter realizes instances by inserting a Realize Instances node into the object's node tree; on the
coconut tree (17 463 leaflet instances) Blender 4.2 crashes there (segfault), in the generation process and in a
separate one alike, although evaluating the same tree for rendering works. This script reads the evaluated
depsgraph instead and appends every instance's mesh, placed by its world matrix, to flat numpy arrays (positions,
triangles, UVs, material indices) built into a single mesh at the end: memory stays close to the size of the
result (a first version copied each instance through a temporary mesh and bmesh and ran the PC out of memory).
The node-tree object is replaced, so the exporter only has to bake textures and write the FBX.

Run inside the Infinigen environment in WSL:
    python realize_instances.py <scene.blend> <output.blend>
"""

import sys

import bpy  # first: with bpy installed as a Python module, bmesh/mathutils only exist once bpy is loaded
import numpy as np


def mesh_arrays(mesh):
    """Local positions, triangle vertex indices, per-corner UVs and per-triangle material slots of a mesh."""
    mesh.calc_loop_triangles()
    count = len(mesh.vertices)
    positions = np.empty(count * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    positions = positions.reshape(-1, 3)

    triangles = len(mesh.loop_triangles)
    vertices = np.empty(triangles * 3, dtype=np.int32)
    mesh.loop_triangles.foreach_get("vertices", vertices)
    loops = np.empty(triangles * 3, dtype=np.int32)
    mesh.loop_triangles.foreach_get("loops", loops)
    materials = np.empty(triangles, dtype=np.int32)
    mesh.loop_triangles.foreach_get("material_index", materials)

    uv = np.zeros((triangles * 3, 2), dtype=np.float32)
    if mesh.uv_layers.active:
        corner_uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
        mesh.uv_layers.active.data.foreach_get("uv", corner_uv)
        uv = corner_uv.reshape(-1, 2)[loops]
    return positions, vertices, uv, materials


def realize(scene_path, output_path):
    bpy.ops.wm.open_mainfile(filepath=scene_path)
    depsgraph = bpy.context.evaluated_depsgraph_get()

    sources = [obj for obj in bpy.context.scene.objects
               if obj.type == "MESH" and any(m.type == "NODES" for m in obj.modifiers)]
    for source in sources:
        all_positions, all_triangles, all_uvs, all_materials = [], [], [], []
        materials, material_index = [], {}
        offset = 0
        parts = 0
        # The leaflets are a handful of meshes instanced thousands of times: convert each distinct mesh once (read
        # straight from the evaluated instance, no to_mesh() copy) and only transform it per instance.
        cache = {}
        for instance in depsgraph.object_instances:
            owner = instance.parent.original if instance.is_instance and instance.parent else instance.object.original
            if owner != source:
                continue
            evaluated = instance.object
            if evaluated.type != "MESH" or evaluated.data is None or len(evaluated.data.polygons) == 0:
                continue
            mesh = evaluated.data
            key = mesh.as_pointer()
            if key not in cache:
                local, vertices, uv, slots = mesh_arrays(mesh)
                # Remap this mesh's material slots into the merged object's slot list.
                remap = []
                for slot in mesh.materials:
                    name = slot.name if slot else ""
                    if name not in material_index:
                        material_index[name] = len(materials)
                        materials.append(slot)
                    remap.append(material_index[name])
                remap = np.array(remap or [0], dtype=np.int32)
                cache[key] = (local, vertices, uv, remap[np.minimum(slots, len(remap) - 1)])
            local, vertices, uv, slots = cache[key]
            m = np.array(instance.matrix_world, dtype=np.float32)
            all_positions.append(local @ m[:3, :3].T + m[:3, 3])
            all_triangles.append(vertices + offset)
            all_uvs.append(uv)
            all_materials.append(slots)
            offset += len(local)
            parts += 1
            if parts % 2000 == 0:
                print(f"realize_instances: {parts} parts, {offset} vertices, {len(cache)} distinct meshes", flush=True)

        positions = np.concatenate(all_positions)
        triangles = np.concatenate(all_triangles).reshape(-1, 3)
        result = bpy.data.meshes.new(source.name + "_realized")
        result.vertices.add(len(positions))
        result.vertices.foreach_set("co", positions.ravel())
        result.loops.add(triangles.size)
        result.loops.foreach_set("vertex_index", triangles.ravel())
        result.polygons.add(len(triangles))
        result.polygons.foreach_set("loop_start", np.arange(0, triangles.size, 3, dtype=np.int32))
        result.polygons.foreach_set("loop_total", np.full(len(triangles), 3, dtype=np.int32))
        result.polygons.foreach_set("material_index", np.concatenate(all_materials))
        result.uv_layers.new(name="UVMap").data.foreach_set("uv", np.concatenate(all_uvs).ravel())
        result.update(calc_edges=True)
        result.validate()
        for material in materials:
            result.materials.append(material)

        realized = bpy.data.objects.new(source.name + "_realized", result)
        bpy.context.scene.collection.objects.link(realized)
        print(f"realize_instances: {source.name}: {parts} parts, {len(triangles)} triangles, {len(materials)} materials")
        bpy.data.objects.remove(source, do_unlink=True)

    bpy.ops.wm.save_as_mainfile(filepath=output_path)


if __name__ == "__main__":
    realize(sys.argv[1], sys.argv[2])
