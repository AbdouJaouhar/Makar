"""
Decimates an Infinigen FBX export to a triangle budget before it goes into Unreal, in place.

Infinigen's film-quality meshes are far too dense for a game (a boulder: 1 M triangles, a 48 MB asset). Nanite
copes with dense meshes, but the repository should not carry them: the baked normal map keeps the fine detail.

Run inside the Infinigen environment in WSL (bpy is installed there as a Python module):
    python decimate_fbx.py <file.fbx> <max_triangles>
"""

import sys

import bpy


def main(path, max_triangles):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path)
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    total = sum(sum(len(p.vertices) - 2 for p in obj.data.polygons) for obj in meshes)
    ratio = min(1.0, max_triangles / max(total, 1))
    print(f"decimate_fbx: {path}: {total} triangles, ratio {ratio:.3f}")
    if ratio < 1.0:
        for obj in meshes:
            modifier = obj.modifiers.new("Decimate", "DECIMATE")
            modifier.decimate_type = "COLLAPSE"
            modifier.ratio = ratio
            modifier.use_collapse_triangulate = True
            bpy.context.view_layer.objects.active = obj
            bpy.ops.object.modifier_apply(modifier="Decimate")
    after = sum(sum(len(p.vertices) - 2 for p in obj.data.polygons) for obj in meshes)
    # Textures stay next to the FBX, referenced by relative path as in Infinigen's own export.
    bpy.ops.export_scene.fbx(filepath=path, use_selection=False, path_mode="RELATIVE", embed_textures=False,
                             mesh_smooth_type="FACE", add_leaf_bones=False)
    print(f"decimate_fbx: {path}: now {after} triangles")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]))
