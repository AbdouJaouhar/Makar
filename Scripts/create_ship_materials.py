"""
Builds the materials of the generated ships (AProceduralShip, Source/Makar/Private/Ship), one per mesh section:
/Game/Ship/M_ShipWood, M_ShipSail, M_ShipRope, M_ShipMetal, M_ShipGloss; and the shipyard's floor, M_ShipyardFloor.
Colours come from the vertex colours (linear albedo); the detail from Shaders/Ship.ush.

Run from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_ship_materials.py
"""

import unreal

DIRECTORY = "/Game/Ship"
MEL = unreal.MaterialEditingLibrary


def fresh_material(name):
    path = f"{DIRECTORY}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DIRECTORY, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("used_with_nanite", True)
    return material


def node(material, cls, x, y):
    return MEL.create_material_expression(material, cls, x, y)


def constant(material, value, x, y):
    expression = node(material, unreal.MaterialExpressionConstant, x, y)
    expression.set_editor_property("r", value)
    return expression


def connect(source, target, input_name, source_output=""):
    if not MEL.connect_material_expressions(source, source_output, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()} -> {target.get_name()}.{input_name}")


def custom(material, description, code, inputs, outputs, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    """A Custom node calling Shaders/Ship.ush: inputs = {name: (expression, output pin)}, outputs = {name: type}."""
    expression = node(material, unreal.MaterialExpressionCustom, -500, 0)
    expression.set_editor_property("description", description)
    expression.set_editor_property("include_file_paths", ["/Makar/Ship.ush"])
    expression.set_editor_property("output_type", output_type)
    expression.set_editor_property("code", code)
    pins = []
    for name in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", name)
        pins.append(pin)
    expression.set_editor_property("inputs", pins)
    extra = []
    for name, kind in outputs.items():
        output = unreal.CustomOutput()
        output.set_editor_property("output_name", name)
        output.set_editor_property("output_type", kind)
        extra.append(output)
    expression.set_editor_property("additional_outputs", extra)
    for name, (source, pin) in inputs.items():
        connect(source, expression, name, pin)
    return expression


def common_inputs(material):
    uv = node(material, unreal.MaterialExpressionTextureCoordinate, -900, 0)
    vertex = node(material, unreal.MaterialExpressionVertexColor, -900, 120)
    world = node(material, unreal.MaterialExpressionWorldPosition, -900, 260)
    return uv, vertex, world


def save(material, name):
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(f"{DIRECTORY}/{name}")


F1 = unreal.CustomMaterialOutputType.CMOT_FLOAT1
F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT3


def wood():
    material = fresh_material("M_ShipWood")
    uv, vertex, world = common_inputs(material)
    shade = custom(material, "Ship Wood", "return ShipWood(UV, Color.rgb, WorldPos, Roughness);",
                   {"UV": (uv, ""), "Color": (vertex, ""), "WorldPos": (world, "")}, {"Roughness": F1})
    MEL.connect_material_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(shade, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.4, -300, 300), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_ShipWood")


def sail():
    material = fresh_material("M_ShipSail")
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    uv, vertex, _ = common_inputs(material)
    shade = custom(material, "Ship Canvas", "return ShipCanvas(UV, Color.rgb, Subsurface, Roughness);",
                   {"UV": (uv, ""), "Color": (vertex, "")}, {"Subsurface": F3, "Roughness": F1})
    MEL.connect_material_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(shade, "Subsurface", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    MEL.connect_material_property(shade, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.3, -300, 300), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_ShipSail")


def rope():
    material = fresh_material("M_ShipRope")
    uv, vertex, _ = common_inputs(material)
    shade = custom(material, "Ship Rope", "return ShipRope(UV, Color.rgb, Roughness);",
                   {"UV": (uv, ""), "Color": (vertex, "")}, {"Roughness": F1})
    MEL.connect_material_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(shade, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.3, -300, 300), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_ShipRope")


def metal():
    material = fresh_material("M_ShipMetal")
    uv, vertex, world = common_inputs(material)
    shade = custom(material, "Ship Metal", "return ShipMetal(UV, float4(Color.rgb, Alpha), WorldPos, Metallic, Roughness);",
                   {"UV": (uv, ""), "Color": (vertex, ""), "Alpha": (vertex, "A"), "WorldPos": (world, "")},
                   {"Metallic": F1, "Roughness": F1})
    MEL.connect_material_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(shade, "Metallic", unreal.MaterialProperty.MP_METALLIC)
    MEL.connect_material_property(shade, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_ShipMetal")


def gloss():
    material = fresh_material("M_ShipGloss")
    vertex = node(material, unreal.MaterialExpressionVertexColor, -600, 0)
    MEL.connect_material_property(vertex, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(constant(material, 0.06, -300, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(constant(material, 0.5, -300, 300), "", unreal.MaterialProperty.MP_SPECULAR)
    save(material, "M_ShipGloss")


def floor():
    """The shipyard's ground: a neutral, slightly mottled grey (18 % albedo, like a grey card), matte."""
    material = fresh_material("M_ShipyardFloor")
    world = node(material, unreal.MaterialExpressionWorldPosition, -900, 0)
    shade = custom(material, "Shipyard Floor",
                   "float N = ShipFbm(WorldPos.xy * 0.004, 4); float F = ShipNoise(WorldPos.xy * 0.08);"
                   " Roughness = 0.8 + 0.1 * F; return float3(0.18, 0.18, 0.175) * lerp(0.85, 1.12, N) * lerp(0.95, 1.03, F);",
                   {"WorldPos": (world, "")}, {"Roughness": F1})
    MEL.connect_material_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(shade, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    save(material, "M_ShipyardFloor")


wood()
sail()
rope()
metal()
gloss()
floor()
unreal.log(f"Ship materials written to {DIRECTORY}")
