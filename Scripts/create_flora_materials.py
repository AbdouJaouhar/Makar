"""
Builds the materials of the procedural flora and rocks (AOceanIslandFlora, Source/Makar/.../OceanFlora.cpp):
/Game/Ocean/Flora/M_Bark, M_Foliage and M_Rock. Colours come from the generated vertex colours.

Run before create_ocean_map.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_flora_materials.py
"""

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


bark()
foliage()
rock()
unreal.log(f"Flora materials written to {DIRECTORY}")
