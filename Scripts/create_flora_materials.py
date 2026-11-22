"""
Builds the materials of the procedural flora and rocks (AOceanIslandFlora, Source/Makar/.../OceanFlora.cpp):
/Game/Ocean/Flora/M_Bark, M_Foliage, M_Rock and M_PalmFrond (with its frond textures). Colours come from the
generated vertex colours.

Run before create_ocean_map.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_flora_materials.py
"""

import os

import unreal

DIRECTORY = "/Game/Ocean/Flora"
MEL = unreal.MaterialEditingLibrary


def fresh_material(name):
    path = f"{DIRECTORY}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DIRECTORY, unreal.Material, unreal.MaterialFactoryNew())


def node(material, cls, x, y):
    return MEL.create_material_expression(material, cls, x, y)


def constant(material, value, x, y):
    expression = node(material, unreal.MaterialExpressionConstant, x, y)
    expression.set_editor_property("r", value)
    return expression


def connect(source, target, input_name, source_output=""):
    if not MEL.connect_material_expressions(source, source_output, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()} -> {target.get_name()}.{input_name}")


def save(material, name):
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(f"{DIRECTORY}/{name}")


def bark():
    material = fresh_material("M_Bark")
    material.set_editor_property("two_sided", True)
    color = node(material, unreal.MaterialExpressionVertexColor, -400, 0)
    MEL.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(constant(material, 0.9, -400, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_Bark")


def foliage():
    """Leaves let light through: two-sided foliage shading, the transmitted light a warmer, yellower green."""
    material = fresh_material("M_Foliage")
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    color = node(material, unreal.MaterialExpressionVertexColor, -600, 0)
    MEL.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    tint = node(material, unreal.MaterialExpressionConstant3Vector, -600, 200)
    # Light through the leaves: a darker, yellower green (a bright tint makes the crowns look pale and plastic).
    tint.set_editor_property("constant", unreal.LinearColor(0.35, 0.5, 0.1, 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -300, 150)
    connect(color, transmitted, "A")
    connect(tint, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, 0.65, -300, 300), "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.3, -300, 380), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_Foliage")


def rock():
    """Coral limestone: vertex colour (lumps, hollows, algae) times a triplanar pitted detail."""
    material = fresh_material("M_Rock")
    material.set_editor_property("two_sided", True)
    color = node(material, unreal.MaterialExpressionVertexColor, -700, 0)
    world_pos = node(material, unreal.MaterialExpressionWorldPosition, -900, 200)
    detail = node(material, unreal.MaterialExpressionCustom, -600, 200)
    detail.set_editor_property("description", "Rock Detail")
    detail.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    detail.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    detail.set_editor_property("code", (
        "const float3 P = WorldPos * 0.01;"
        "const float N = (OceanValueNoise(P.xy * 1.7) + OceanValueNoise(P.xz * 1.7 + 7.0) + OceanValueNoise(P.yz * 1.7 + 13.0)) / 3.0;"
        "const float Pits = (OceanValueNoise(P.xy * 9.0) + OceanValueNoise(P.xz * 9.0 + 3.0) + OceanValueNoise(P.yz * 9.0 + 5.0)) / 3.0;"
        "return 0.65 + 0.5 * N - 0.25 * smoothstep(0.55, 0.75, Pits);"))
    custom_input = unreal.CustomInput()
    custom_input.set_editor_property("input_name", "WorldPos")
    detail.set_editor_property("inputs", [custom_input])
    connect(world_pos, detail, "WorldPos")
    base = node(material, unreal.MaterialExpressionMultiply, -300, 100)
    connect(color, base, "A")
    connect(detail, base, "B")
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(constant(material, 0.85, -300, 300), "", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_Rock")


FROND_SOURCE = os.path.join(unreal.Paths.project_saved_dir(), "Infinigen", "frond")


def import_texture(path, name, normal_map):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", DIRECTORY)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.EditorAssetLibrary.load_asset(f"{DIRECTORY}/{name}")
    if normal_map:
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        texture.set_editor_property("srgb", False)
        # The frond was rendered with its tip up the image (+Y), Unreal's normal maps point green down (+V).
        texture.set_editor_property("flip_green_channel", True)
    unreal.EditorAssetLibrary.save_asset(f"{DIRECTORY}/{name}")
    return texture


def palm_frond():
    """Coconut palm leaf cards: a frond rendered from Infinigen's coconut palm (Scripts/infinigen/
    render_frond_atlas.py), cut out by its alpha, lit as two-sided foliage. The vertex colour tints it: young
    fronds keep the rendered green, old drooping ones yellow."""
    if not os.path.isfile(os.path.join(FROND_SOURCE, "frond_basecolor.png")):
        unreal.log_warning(f"No frond textures in {FROND_SOURCE}: M_PalmFrond not built (palms fall back to M_Foliage)")
        return
    color_texture = import_texture(os.path.join(FROND_SOURCE, "frond_basecolor.png"), "T_PalmFrond_BaseColor", False)
    normal_texture = import_texture(os.path.join(FROND_SOURCE, "frond_normal.png"), "T_PalmFrond_Normal", True)

    material = fresh_material("M_PalmFrond")
    material.set_editor_property("two_sided", True)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", 0.4)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)

    color = node(material, unreal.MaterialExpressionTextureSample, -900, 0)
    color.set_editor_property("texture", color_texture)
    normal = node(material, unreal.MaterialExpressionTextureSample, -900, 400)
    normal.set_editor_property("texture", normal_texture)
    normal.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)

    # Slightly less saturated than the render, then tinted by age: vertex colour over the young frond green.
    desaturate = node(material, unreal.MaterialExpressionDesaturation, -650, 0)
    connect(color, desaturate, "", "RGB")  # first input, unnamed
    connect(constant(material, 0.2, -800, 120), desaturate, "Fraction")
    vertex = node(material, unreal.MaterialExpressionVertexColor, -900, 250)
    young = node(material, unreal.MaterialExpressionConstant3Vector, -900, 330)
    young.set_editor_property("constant", unreal.LinearColor(0.12, 0.2, 0.04, 1.0))
    ratio = node(material, unreal.MaterialExpressionDivide, -650, 250)
    connect(vertex, ratio, "A")
    connect(young, ratio, "B")
    tint = node(material, unreal.MaterialExpressionClamp, -500, 250)
    connect(ratio, tint, "")
    tint.set_editor_property("max_default", 1.6)
    base = node(material, unreal.MaterialExpressionMultiply, -350, 100)
    connect(desaturate, base, "A")
    connect(tint, base, "B")
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(color, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)

    # Light through the leaflets: a warmer, yellower green.
    transmit = node(material, unreal.MaterialExpressionConstant3Vector, -500, 450)
    transmit.set_editor_property("constant", unreal.LinearColor(0.6, 0.8, 0.25, 1.0))
    transmitted = node(material, unreal.MaterialExpressionMultiply, -250, 400)
    connect(base, transmitted, "A")
    connect(transmit, transmitted, "B")
    MEL.connect_material_property(transmitted, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(constant(material, 0.55, -250, 550), "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.4, -250, 620), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_PalmFrond")


bark()
foliage()
rock()
palm_frond()
unreal.log(f"Flora materials written to {DIRECTORY}")
