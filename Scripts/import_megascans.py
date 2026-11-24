"""
Imports Quixel Megascans plants (Fab, FBX, quality "High") as Nanite static meshes with a wind-driven foliage
material: /Game/Megascans/<Name>/SM_<Name>_Var<X> and M_<Name>. AOceanIslandFlora places them
(ScannedUnderstoryMeshes).

The source folders are Fab's downloads (Epic Games Launcher > Fab library): set them in ASSETS. Run headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/import_megascans.py
"""

import glob
import os

import unreal

FAB = r"C:\ProgramData\Epic\EpicGamesLauncher\VaultCache\FabLibrary"
# (asset name in the project, Fab folder, file prefix): Megascans' files are <prefix>_4K_<Map>.jpg and
# <prefix>_<id>_Var<X>_LOD0.fbx.
ASSETS = [
    ("TropicalPalm", os.path.join(FAB, r"Tropical_Palm-a14792bb\fbx\high\tropical_palm_rkqjj_high_extracted"),
     "Tropical_Palm_rkqjj_High"),
]
ROOT = "/Game/Megascans"
MEL = unreal.MaterialEditingLibrary
# Shaders/Flora.ush: the same wind law as the procedural flora (Scripts/create_flora_materials.py, add_wind).
WIND_BEND = 0.35


def run_task(path, destination, options=None, name=None):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", destination)
    if name:
        task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    if options:
        task.set_editor_property("options", options)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    paths = task.get_editor_property("imported_object_paths")
    if not paths:
        raise RuntimeError(f"Could not import {path}")
    return unreal.load_asset(paths[0])


def import_texture(path, destination, name, srgb=True, normal=False, mask=False):
    texture = run_task(path, destination, name=name)
    texture.set_editor_property("srgb", srgb)
    if normal:
        # Megascans' normal maps are OpenGL (+Y): Unreal's are DirectX.
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        texture.set_editor_property("flip_green_channel", True)
    elif mask:
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_GRAYSCALE)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)
    return texture


def import_mesh(path, destination, name):
    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_as_skeletal", False)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_STATIC_MESH)
    mesh_options = options.get_editor_property("static_mesh_import_data")
    mesh_options.set_editor_property("combine_meshes", True)
    mesh_options.set_editor_property("build_nanite", True)
    mesh_options.set_editor_property("generate_lightmap_u_vs", False)
    mesh_options.set_editor_property("auto_generate_collision", False)
    return run_task(path, destination, options, name)


def node(material, cls, x, y):
    return MEL.create_material_expression(material, cls, x, y)


def connect(source, target, input_name, source_output=""):
    if not MEL.connect_material_expressions(source, source_output, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()} -> {target.get_name()}.{input_name}")


def sample(material, texture, x, y, kind=None):
    expression = node(material, unreal.MaterialExpressionTextureSample, x, y)
    expression.set_editor_property("texture", texture)
    if kind:
        expression.set_editor_property("sampler_type", kind)
    return expression


def add_wind(material, flutter):
    """The procedural flora's wind (Shaders/Flora.ush). No palm-frond data on scanned meshes: Frond = 0 (their UV
    channel 1 is not wind data)."""
    wind = node(material, unreal.MaterialExpressionCustom, -600, 900)
    wind.set_editor_property("description", "Flora Wind")
    wind.set_editor_property("include_file_paths", ["/Makar/Flora.ush"])
    wind.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    wind.set_editor_property("code", f"return FloraWind(WorldPos, Pivot, Local, float4(0, 0, 0, 0), Time, {WIND_BEND}, {flutter});")
    inputs = []
    for input_name in ("WorldPos", "Pivot", "Local", "Time"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        inputs.append(custom_input)
    wind.set_editor_property("inputs", inputs)
    connect(node(material, unreal.MaterialExpressionWorldPosition, -900, 900), wind, "WorldPos")
    pivot = node(material, unreal.MaterialExpressionTransformPosition, -750, 820)
    pivot.set_editor_property("transform_source_type", unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
    pivot.set_editor_property("transform_type", unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD)
    connect(node(material, unreal.MaterialExpressionConstant3Vector, -900, 820), pivot, "")
    connect(pivot, wind, "Pivot")
    connect(node(material, unreal.MaterialExpressionLocalPosition, -900, 980), wind, "Local")
    connect(node(material, unreal.MaterialExpressionTime, -900, 1060), wind, "Time")
    MEL.connect_material_property(wind, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    material.set_editor_property("max_world_position_offset_displacement", 60.0)


def build_material(destination, name, textures):
    """Two-sided foliage, the leaves cut out by the scan's opacity, lit through by its translucency map."""
    path = f"{destination}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, destination, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("used_with_instanced_static_meshes", True)
    material.set_editor_property("used_with_nanite", True)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", 0.33)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    color = sample(material, textures["BaseColor"], -900, 0)
    MEL.connect_material_property(color, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    normal = sample(material, textures["Normal"], -900, 250, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    MEL.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    roughness = sample(material, textures["Roughness"], -900, 500, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    MEL.connect_material_property(roughness, "RGB", unreal.MaterialProperty.MP_ROUGHNESS)
    opacity = sample(material, textures["Opacity"], -900, 650, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    MEL.connect_material_property(opacity, "RGB", unreal.MaterialProperty.MP_OPACITY_MASK)
    translucency = sample(material, textures["Translucency"], -900, 800)
    through = node(material, unreal.MaterialExpressionMultiply, -500, 750)
    connect(translucency, through, "A", "RGB")
    connect(color, through, "B", "RGB")
    MEL.connect_material_property(through, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    specular = node(material, unreal.MaterialExpressionConstant, -300, 600)
    specular.set_editor_property("r", 0.4)
    MEL.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)
    add_wind(material, 0.8)
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


def main():
    for name, folder, prefix in ASSETS:
        if not os.path.isdir(folder):
            unreal.log_warning(f"import_megascans: {folder} not found, {name} skipped")
            continue
        destination = f"{ROOT}/{name}"
        textures = {}
        for kind, srgb, normal, mask in (("BaseColor", True, False, False), ("Normal", False, True, False),
                                        ("Roughness", False, False, True), ("Opacity", False, False, True),
                                        ("Translucency", True, False, False)):
            files = glob.glob(os.path.join(folder, f"{prefix}_*K_{kind}.jpg"))
            if not files:
                raise RuntimeError(f"import_megascans: no {kind} map in {folder}")
            textures[kind] = import_texture(files[0], destination, f"T_{name}_{kind}", srgb, normal, mask)
        material = build_material(destination, f"M_{name}", textures)
        meshes = sorted(glob.glob(os.path.join(folder, f"{prefix}_*_Var*_LOD0.fbx")))
        for path in meshes:
            variant = path.split("_Var")[1].split("_")[0]
            mesh = import_mesh(path, destination, f"SM_{name}_Var{variant}")
            for index in range(len(mesh.get_editor_property("static_materials"))):
                mesh.set_material(index, material)
            unreal.EditorAssetLibrary.save_loaded_asset(mesh)
        unreal.log(f"import_megascans: {name}: {len(meshes)} variants, {len(textures)} maps into {destination}")


main()
