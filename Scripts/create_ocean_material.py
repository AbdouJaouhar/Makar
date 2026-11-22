"""
Builds /Game/Ocean/M_Ocean, the material used by AOceanActor.

Run once in the Unreal editor (and again after changing this script):
    Tools > Execute Python Script... > Scripts/create_ocean_material.py
The HLSL lives in Shaders/Ocean.ush: editing it only needs a shader recompile, not this script.
"""

import unreal

ASSET_DIR = "/Game/Ocean"
ASSET_NAME = "M_Ocean"
ASSET_PATH = f"{ASSET_DIR}/{ASSET_NAME}"
INCLUDE = "/Makar/Ocean.ush"

MEL = unreal.MaterialEditingLibrary


def get_or_create_material():
    # Start from a fresh asset: delete_all_material_expressions keeps the Single Layer Water output node,
    # and a second one makes the material fail to compile.
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(ASSET_PATH):
            raise RuntimeError(f"Could not delete {ASSET_PATH} to rebuild it")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    return tools.create_asset(ASSET_NAME, ASSET_DIR, unreal.Material, unreal.MaterialFactoryNew())


def placeholder_texture():
    for path in ("/Engine/EngineResources/Black.Black", "/Engine/EngineResources/DefaultTexture.DefaultTexture"):
        texture = unreal.load_asset(path)
        if texture:
            return texture
    raise RuntimeError("No placeholder texture found")


def node(material, cls, x, y):
    return MEL.create_material_expression(material, cls, x, y)


def scalar(material, name, value, x, y):
    expression = node(material, unreal.MaterialExpressionScalarParameter, x, y)
    expression.set_editor_property("parameter_name", name)
    expression.set_editor_property("default_value", value)
    return expression


def color(material, name, rgb, x, y):
    expression = node(material, unreal.MaterialExpressionVectorParameter, x, y)
    expression.set_editor_property("parameter_name", name)
    expression.set_editor_property("default_value", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
    return expression


def texture(material, name, placeholder, x, y):
    expression = node(material, unreal.MaterialExpressionTextureObjectParameter, x, y)
    expression.set_editor_property("parameter_name", name)
    expression.set_editor_property("texture", placeholder)
    return expression


def custom(material, x, y, description, code, inputs, output_type, extra_outputs=()):
    expression = node(material, unreal.MaterialExpressionCustom, x, y)
    expression.set_editor_property("description", description)
    expression.set_editor_property("code", code)
    expression.set_editor_property("output_type", output_type)
    expression.set_editor_property("include_file_paths", [INCLUDE])

    custom_inputs = []
    for input_name in inputs:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        custom_inputs.append(custom_input)
    expression.set_editor_property("inputs", custom_inputs)

    custom_outputs = []
    for output_name, output_kind in extra_outputs:
        custom_output = unreal.CustomOutput()
        custom_output.set_editor_property("output_name", output_name)
        custom_output.set_editor_property("output_type", output_kind)
        custom_outputs.append(custom_output)
    expression.set_editor_property("additional_outputs", custom_outputs)
    return expression


def connect(source, target, input_name, source_output=""):
    if not MEL.connect_material_expressions(source, source_output, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()}.{source_output} -> {target.get_name()}.{input_name}")


def connect_inputs(sources, target):
    for input_name, source in sources.items():
        connect(source, target, input_name)


def build():
    material = get_or_create_material()
    material.set_editor_property("tangent_space_normal", False)  # the ocean computes world-space normals
    # Single Layer Water: light is absorbed and scattered through the water volume down to the seabed,
    # which is what gives clear tropical water its turquoise-to-deep-blue gradient.
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
    material.set_editor_property("two_sided", False)

    placeholder = placeholder_texture()

    # --- Shared inputs ---
    world_pos = node(material, unreal.MaterialExpressionWorldPosition, -1600, 0)
    world_pos.set_editor_property(
        "world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    camera_pos = node(material, unreal.MaterialExpressionCameraPositionWS, -1600, 120)

    disp = [texture(material, f"Disp{i}", placeholder, -1600, 260 + i * 140) for i in range(3)]
    deriv = [texture(material, f"Deriv{i}", placeholder, -1600, 700 + i * 140) for i in range(3)]
    moments = [texture(material, f"Moments{i}", placeholder, -1600, 1140 + i * 140) for i in range(3)]
    # Simulated foam around the camera; FoamMapSize 0 (until the first frame) disables it.
    foam_map = texture(material, "FoamMap", placeholder, -1600, 1560)
    foam_map_origin = color(material, "FoamMapOrigin", (0.0, 0.0, 0.0), -1300, 1160)
    foam_map_size = scalar(material, "FoamMapSize", 0.0, -1300, 1240)
    # Coastal data from AOceanBathymetry (depth, distance to shore, direction); ShoreMapSize 0 = open sea.
    shore_map = texture(material, "ShoreMap", placeholder, -1600, 1700)
    shore_map_origin = color(material, "ShoreMapOrigin", (0.0, 0.0, 0.0), -1300, 1320)
    shore_map_size = scalar(material, "ShoreMapSize", 0.0, -1300, 1400)
    # Per-cascade significant wave height, updated every frame: limits waves in shallow water.
    sig_height = [scalar(material, f"SignificantHeight{i}", 1.0, -1300, 1480 + i * 80) for i in range(3)]
    # Swell breaking on the beach: simulation time (set every frame, so it matches the CPU queries) and swell.
    ocean_time = scalar(material, "OceanTime", 0.0, -1300, 1720)
    swell_height = scalar(material, "ShoreSwellHeight", 0.8, -1300, 1800)
    swell_period = scalar(material, "ShoreSwellPeriod", 8.0, -1300, 1880)
    swell_max_depth = scalar(material, "ShoreSwellMaxDepth", 12.0, -1300, 1960)
    swell_inputs = {"Time": ocean_time, "SwellHeight": swell_height, "SwellPeriod": swell_period,
                    "SwellMaxDepth": swell_max_depth}

    patch = [scalar(material, f"PatchSize{i}", v, -1300, 260 + i * 80) for i, v in enumerate((500.0, 85.0, 10.0))]
    resolution = scalar(material, "Resolution", 128.0, -1300, 520)
    grid_spacing = scalar(material, "GridSpacing", 0.5, -1300, 600)
    grid_inner = scalar(material, "GridInnerHalfWidth", 64.0, -1300, 680)
    grid_growth = scalar(material, "GridGrowth", 0.13, -1300, 760)
    fade_start = scalar(material, "FadeStart", 3000.0, -1300, 840)
    fade_end = scalar(material, "FadeEnd", 8000.0, -1300, 920)
    crest_height = scalar(material, "CrestHeight", 1.5, -1300, 1000)

    # --- Vertex: displacement ---
    vertex = custom(
        material, -900, -200, "Ocean Vertex Offset",
        "return OceanVertexOffset(WorldPos, CameraPos, Disp0, Disp0Sampler, Disp1, Disp1Sampler, "
        "ShoreMap, ShoreMapSampler, ShoreMapOrigin, ShoreMapSize, SignificantHeight0, SignificantHeight1, "
        "Time, SwellHeight, SwellPeriod, SwellMaxDepth, "
        "PatchSize0, PatchSize1, Resolution, GridSpacing, GridInnerHalfWidth, GridGrowth, FadeStart, FadeEnd);",
        ["WorldPos", "CameraPos", "Disp0", "Disp1",
         "ShoreMap", "ShoreMapOrigin", "ShoreMapSize", "SignificantHeight0", "SignificantHeight1",
         "Time", "SwellHeight", "SwellPeriod", "SwellMaxDepth",
         "PatchSize0", "PatchSize1", "Resolution",
         "GridSpacing", "GridInnerHalfWidth", "GridGrowth", "FadeStart", "FadeEnd"],
        unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    connect_inputs({
        "WorldPos": world_pos, "CameraPos": camera_pos, "Disp0": disp[0], "Disp1": disp[1],
        "ShoreMap": shore_map, "ShoreMapOrigin": shore_map_origin, "ShoreMapSize": shore_map_size,
        "SignificantHeight0": sig_height[0], "SignificantHeight1": sig_height[1], **swell_inputs,
        "PatchSize0": patch[0], "PatchSize1": patch[1], "Resolution": resolution,
        "GridSpacing": grid_spacing, "GridInnerHalfWidth": grid_inner, "GridGrowth": grid_growth,
        "FadeStart": fade_start, "FadeEnd": fade_end,
    }, vertex)
    MEL.connect_material_property(vertex, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    # Micro-roughness of the water itself; the shader adds the roughness of waves too small to be resolved.
    roughness_water = scalar(material, "Roughness", 0.04, -1300, 1080)

    # --- Pixel: normal, foam, crest, filtered roughness ---
    pixel = custom(
        material, -900, 400, "Ocean Pixel",
        "return OceanPixel(WorldPos, Disp0, Disp0Sampler, Disp1, Disp1Sampler, Disp2, Disp2Sampler, "
        "Deriv0, Deriv0Sampler, Deriv1, Deriv1Sampler, Deriv2, Deriv2Sampler, "
        "Moments0, Moments0Sampler, Moments1, Moments1Sampler, Moments2, Moments2Sampler, "
        "FoamMap, FoamMapSampler, FoamMapOrigin, FoamMapSize, "
        "ShoreMap, ShoreMapSampler, ShoreMapOrigin, ShoreMapSize, "
        "SignificantHeight0, SignificantHeight1, SignificantHeight2, "
        "Time, SwellHeight, SwellPeriod, SwellMaxDepth, "
        "PatchSize0, PatchSize1, PatchSize2, CrestHeight, BaseRoughness, Foam, Crest, Roughness);",
        ["WorldPos", "Disp0", "Disp1", "Disp2", "Deriv0", "Deriv1", "Deriv2", "Moments0", "Moments1", "Moments2",
         "FoamMap", "FoamMapOrigin", "FoamMapSize",
         "ShoreMap", "ShoreMapOrigin", "ShoreMapSize",
         "SignificantHeight0", "SignificantHeight1", "SignificantHeight2",
         "Time", "SwellHeight", "SwellPeriod", "SwellMaxDepth",
         "PatchSize0", "PatchSize1", "PatchSize2", "CrestHeight", "BaseRoughness"],
        unreal.CustomMaterialOutputType.CMOT_FLOAT3,
        extra_outputs=[("Foam", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
                       ("Crest", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
                       ("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1)])
    connect_inputs({
        "WorldPos": world_pos, "Disp0": disp[0], "Disp1": disp[1], "Disp2": disp[2],
        "Deriv0": deriv[0], "Deriv1": deriv[1], "Deriv2": deriv[2],
        "Moments0": moments[0], "Moments1": moments[1], "Moments2": moments[2],
        "FoamMap": foam_map, "FoamMapOrigin": foam_map_origin, "FoamMapSize": foam_map_size,
        "ShoreMap": shore_map, "ShoreMapOrigin": shore_map_origin, "ShoreMapSize": shore_map_size,
        "SignificantHeight0": sig_height[0], "SignificantHeight1": sig_height[1],
        "SignificantHeight2": sig_height[2], **swell_inputs,
        "PatchSize0": patch[0], "PatchSize1": patch[1], "PatchSize2": patch[2], "CrestHeight": crest_height,
        "BaseRoughness": roughness_water,
    }, pixel)
    MEL.connect_material_property(pixel, "", unreal.MaterialProperty.MP_NORMAL)

    # --- Surface: the water itself is shaded by the volume below; base colour only carries the foam ---
    foam_color = color(material, "FoamColor", (0.8, 0.85, 0.85), -500, 420)
    base = node(material, unreal.MaterialExpressionMultiply, -200, 340)
    connect(foam_color, base, "A")
    connect(pixel, base, "B", "Foam")
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    # In Single Layer Water, Opacity is the opaque surface coverage (foam): 0 = see into the water volume.
    # Left unconnected it defaults to 1 and the water turns into an opaque black surface.
    MEL.connect_material_property(pixel, "Foam", unreal.MaterialProperty.MP_OPACITY)

    roughness_foam = scalar(material, "FoamRoughness", 0.7, -500, 640)
    roughness = node(material, unreal.MaterialExpressionLinearInterpolate, -200, 580)
    connect(pixel, roughness, "A", "Roughness")
    connect(roughness_foam, roughness, "B")
    connect(pixel, roughness, "Alpha", "Foam")
    MEL.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # Water F0 is ~0.02, i.e. Specular 0.25 in Unreal's convention.
    specular = scalar(material, "Specular", 0.25, -200, 720)
    MEL.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)

    # --- Water volume. Caribbean water is very clear: red is absorbed within a few metres,
    # blue-green travels far and is scattered back, so shallow water over sand reads turquoise
    # and deep water reads saturated blue.
    # Single Layer Water applies these coefficients per CENTIMETRE (checked in-engine: the per-metre
    # values 0.45 / 0.06 / 0.03 made 6 m of water fully opaque). ---
    absorption = color(material, "Absorption", (0.009, 0.001, 0.0005), -500, 800)
    scattering = color(material, "Scattering", (0.0001, 0.0006, 0.0007), -500, 920)

    # Thin wave crests let more light through: boost scattering there.
    crest_boost = scalar(material, "CrestScatterBoost", 2.0, -500, 1040)
    crest_scale = node(material, unreal.MaterialExpressionMultiply, -350, 1000)
    connect(pixel, crest_scale, "A", "Crest")
    connect(crest_boost, crest_scale, "B")
    one = node(material, unreal.MaterialExpressionConstant, -350, 1100)
    one.set_editor_property("r", 1.0)
    crest_factor = node(material, unreal.MaterialExpressionAdd, -250, 1040)
    connect(one, crest_factor, "A")
    connect(crest_scale, crest_factor, "B")
    scattering_final = node(material, unreal.MaterialExpressionMultiply, -150, 940)
    connect(scattering, scattering_final, "A")
    connect(crest_factor, scattering_final, "B")

    # Sand stirred up by the breaking waves makes the surf zone milky (OceanSurfTurbidity): strong, neutral to
    # cream scattering and a little absorption of blue by the grains, per cm like the coefficients above.
    turbidity = custom(
        material, -500, 1320, "Surf Turbidity",
        "return OceanSurfTurbidity(WorldPos, Time, ShoreMap, ShoreMapSampler, ShoreMapOrigin, ShoreMapSize, "
        "SwellHeight, SwellMaxDepth);",
        ["WorldPos", "Time", "ShoreMap", "ShoreMapOrigin", "ShoreMapSize", "SwellHeight", "SwellMaxDepth"],
        unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    connect_inputs({
        "WorldPos": world_pos, "Time": swell_inputs["Time"], "ShoreMap": shore_map, "ShoreMapOrigin": shore_map_origin,
        "ShoreMapSize": shore_map_size, "SwellHeight": swell_inputs["SwellHeight"],
        "SwellMaxDepth": swell_inputs["SwellMaxDepth"],
    }, turbidity)
    sand_scattering = color(material, "SurfSandScattering", (0.006, 0.0058, 0.005), -500, 1440)
    sand_absorption = color(material, "SurfSandAbsorption", (0.0003, 0.0005, 0.0009), -500, 1520)
    milky_scatter = node(material, unreal.MaterialExpressionMultiply, -300, 1440)
    connect(sand_scattering, milky_scatter, "A")
    connect(turbidity, milky_scatter, "B")
    scattering_total = node(material, unreal.MaterialExpressionAdd, -100, 1000)
    connect(scattering_final, scattering_total, "A")
    connect(milky_scatter, scattering_total, "B")
    milky_absorb = node(material, unreal.MaterialExpressionMultiply, -300, 1520)
    connect(sand_absorption, milky_absorb, "A")
    connect(turbidity, milky_absorb, "B")
    absorption_total = node(material, unreal.MaterialExpressionAdd, -100, 820)
    connect(absorption, absorption_total, "A")
    connect(milky_absorb, absorption_total, "B")

    phase_g = scalar(material, "PhaseG", 0.3, -500, 1160)

    water_output = node(material, unreal.MaterialExpressionSingleLayerWaterMaterialOutput, 0, 900)
    connect(scattering_total, water_output, "ScatteringCoefficients")
    connect(absorption_total, water_output, "AbsorptionCoefficients")
    connect(phase_g, water_output, "PhaseG")
    # Explicit 1: what lies under the water (the seabed) is seen through it, only tinted by the volume.
    behind = scalar(material, "ColorScaleBehindWater", 1.0, -500, 1240)
    connect(behind, water_output, "ColorScaleBehindWater")

    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(ASSET_PATH)
    unreal.log(f"Ocean material written to {ASSET_PATH}")


build()
