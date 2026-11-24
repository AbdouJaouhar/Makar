"""
Builds /Game/Maps/Ocean: a Caribbean sea showcase (a sandy island, clear water over a white sand shelf,
a reef drop-off, tropical sun, trade-wind sea state) explored with a free-flying camera.

Run after create_ocean_material.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_ocean_map.py
Running it again rebuilds the level from scratch.
"""

import math
import os

import unreal

MAP_PATH = "/Game/Maps/Ocean"
SEABED_MATERIAL_PATH = "/Game/Ocean/M_Seabed"

# Island (AOceanBathymetry) west of the start: its beach faces the trade-wind waves, which travel towards -X.
# The shelf around it is ~7 m deep and drops into deep water ~450 m from the shore (behind the start).
ISLAND_CENTRE_M = (-1300.0, 0.0)
ISLAND_RADIUS_M = 850.0

# Sky: Mie (aerosol) scattering and absorption scales (engine defaults 0.003996 / 0.000444), height fog density.
SKY_MIE_SCATTERING = 0.0013
SKY_MIE_ABSORPTION = 0.00015
SKY_FOG_DENSITY = 0.0003

# Swell lapping on the beaches of the lagoon (ocean settings and the wet sand band of M_Seabed).
SHORE_SWELL_HEIGHT_M = 0.6  # a trade-wind beach: shore break of ~0.6 m
SHORE_SWELL_PERIOD_S = 7.0
SHORE_SWELL_MAX_DEPTH_M = 12.0

# Resolution of the finest ocean cascade, simulated on the GPU (FOceanSettings::GpuDetailResolution).
GPU_DETAIL_RESOLUTION = 512  # 1024: 2.6 ms of GPU a frame for ripples finer than 2 cm

# Post-process overrides (FPostProcessSettings property names), tuned on screenshots of the beach from the sea:
# without them the sand clips to ~236/230/218; highlight compression alone also darkened the water by half,
# +0.3 EV brings the turquoise back. A little extra saturation for the vivid tropical light.
POST_PROCESS = {
    "local_exposure_highlight_contrast_scale": 0.7,
    "local_exposure_shadow_contrast_scale": 0.8,
    "auto_exposure_bias": 0.3,
    "bloom_intensity": 0.5,
    "color_saturation": unreal.Vector4(1.12, 1.12, 1.12, 1.12),
}

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
MEL = unreal.MaterialEditingLibrary


def load_class(path):
    cls = unreal.load_class(None, path)
    if cls is None:
        raise RuntimeError(f"Class not found: {path} (is the Makar module compiled?)")
    return cls


def spawn(cls, location=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), label=None):
    actor = actors.spawn_actor_from_class(
        cls, unreal.Vector(*location), unreal.Rotator(roll=rotation[0], pitch=rotation[1], yaw=rotation[2]))
    if actor is None:
        raise RuntimeError(f"Could not spawn {cls.get_name()}")
    if label:
        actor.set_actor_label(label)
    return actor


# Time of day: the sun's elevation and the compass direction towards it (degrees, X = 0, Y = 90). Late afternoon:
# low enough for long shadows, warm light through the atmosphere and the palms lit from the side, high enough that the
# lagoon keeps its turquoise. MAKAR_SUN="elevation,azimuth" overrides it (look development).
SUN_ELEVATION_DEG, SUN_AZIMUTH_DEG = (float(v) for v in os.environ.get("MAKAR_SUN", "14,-60").split(","))
SUN_ROTATION = (0.0, -SUN_ELEVATION_DEG, SUN_AZIMUTH_DEG + 180.0)  # roll, pitch, yaw of the directional light


def sun_direction():
    """Unit vector towards the sun (the light travels along the actor's forward axis)."""
    _, pitch, yaw = (math.radians(a) for a in SUN_ROTATION)
    return (-math.cos(pitch) * math.cos(yaw), -math.cos(pitch) * math.sin(yaw), -math.sin(pitch))


GROUND_DIRECTORY = "/Game/Ocean/Ground"
# Tints over the rendered tiles (linear means: limestone 0.017 grey, soil 0.11/0.032/0.014, litter 0.4/0.21/0.09)
# towards pale grey coral limestone (~0.4), brown-red island soil (~0.2/0.1/0.055) and tan dry leaves (~0.24/0.16/0.09).
GROUND_ROCK_TINT = (32.0, 30.5, 26.0)  # pale coral limestone (28 read dark grey on the cliffs, 38 white on the forest banks)
GROUND_SOIL_TINT = (1.8, 3.1, 3.9)
GROUND_LITTER_TINT = (0.42, 0.6, 0.8)  # duller, darker brown (0.6/0.78/0.95 read pink and yellow in the low sun)
GROUND_GRASS_TINT = (1.25, 0.72, 0.5)  # grass 0.057/0.138/0.089, bluish -> a muted green ~0.07/0.1/0.045 (0.11/0.15/0.05 glowed lime)


def import_ground_textures():
    """Imports the ground tiles fetched into Saved/Infinigen/ground (Scripts/infinigen/fetch_exports.ps1) as
    /Game/Ocean/Ground/T_Ground_<Kind>_<BaseColor|Normal>. Returns {(kind, suffix): texture} for those present."""
    source = os.path.join(unreal.Paths.project_saved_dir(), "Infinigen", "ground")
    textures = {}
    for kind in ("limestone", "soil", "litter", "grass", "wrack"):
        for suffix, file_suffix in (("BaseColor", "basecolor"), ("Normal", "normal")):
            path = os.path.join(source, f"{kind}_{file_suffix}.png")
            if not os.path.isfile(path):
                unreal.log_warning(f"No ground tile {path}: the terrain uses a flat placeholder")
                continue
            name = f"T_Ground_{kind.capitalize()}_{suffix}"
            # Already imported from this very file: keep the asset (re-importing rewrites it, and git sees a change).
            stamp = f"{os.path.getmtime(path):.0f}:{os.path.getsize(path)}"
            if unreal.EditorAssetLibrary.does_asset_exist(f"{GROUND_DIRECTORY}/{name}"):
                existing = unreal.EditorAssetLibrary.load_asset(f"{GROUND_DIRECTORY}/{name}")
                if unreal.EditorAssetLibrary.get_metadata_tag(existing, "SourceStamp") == stamp:
                    textures[(kind, suffix)] = existing
                    continue
            task = unreal.AssetImportTask()
            task.set_editor_property("filename", path)
            task.set_editor_property("destination_path", GROUND_DIRECTORY)
            task.set_editor_property("destination_name", name)
            task.set_editor_property("automated", True)
            task.set_editor_property("replace_existing", True)
            task.set_editor_property("save", False)
            unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
            texture = unreal.EditorAssetLibrary.load_asset(f"{GROUND_DIRECTORY}/{name}")
            if suffix == "Normal":
                # Normals in the tile's frame, decoded by the shader (OceanGroundPlane): no green flip here.
                texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
                texture.set_editor_property("srgb", False)
            unreal.EditorAssetLibrary.set_metadata_tag(texture, "SourceStamp", stamp)
            unreal.EditorAssetLibrary.save_asset(f"{GROUND_DIRECTORY}/{name}")
            textures[(kind, suffix)] = texture
    return textures


SPRAY_MATERIAL_PATH = "/Game/Ocean/M_OceanSpray"


def spray_material():
    """Sea spray on the spray curtain (AOceanBathymetry::BuildSprayCurtain): OceanSpray in Shaders/Ocean.ush, unlit and
    translucent, lit by hand with the sun's illuminance through the atmosphere (single scattering by the droplets),
    softened where it meets the water and the rocks (depth fade). The shore data comes from the bathymetry's dynamic
    instance; the swell must match the ocean's (as for the terrain)."""
    if unreal.EditorAssetLibrary.does_asset_exist(SPRAY_MATERIAL_PATH):
        material = unreal.EditorAssetLibrary.load_asset(SPRAY_MATERIAL_PATH)
        MEL.delete_all_material_expressions(material)
    else:
        directory, name = SPRAY_MATERIAL_PATH.rsplit("/", 1)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, directory, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)

    def expression(cls, x, y):
        return MEL.create_material_expression(material, cls, x, y)

    def scalar(name, value, x, y):
        node = expression(unreal.MaterialExpressionScalarParameter, x, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", value)
        return node

    def connect(source, target, input_name, source_output=""):
        if not MEL.connect_material_expressions(source, source_output, target, input_name):
            raise RuntimeError(f"Could not connect {source.get_name()} -> {target.get_name()}.{input_name}")

    spray = expression(unreal.MaterialExpressionCustom, -500, 0)
    spray.set_editor_property("description", "Ocean Spray")
    spray.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    spray.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    spray.set_editor_property("code", "return OceanSpray(WorldPos, Time, Rock, ShoreMap, ShoreMapSampler, ShoreMapOrigin, "
                                      "ShoreMapSize, SwellHeight, SwellPeriod, ToSun, ToCamera, Shoreward, Radiance);")
    inputs = []
    for name in ("WorldPos", "Time", "Rock", "ShoreMap", "ShoreMapOrigin", "ShoreMapSize", "SwellHeight", "SwellPeriod",
                 "ToSun", "ToCamera", "Shoreward"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    spray.set_editor_property("inputs", inputs)
    radiance_output = unreal.CustomOutput()
    radiance_output.set_editor_property("output_name", "Radiance")
    radiance_output.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    spray.set_editor_property("additional_outputs", [radiance_output])

    connect(expression(unreal.MaterialExpressionWorldPosition, -900, -200), spray, "WorldPos")
    connect(expression(unreal.MaterialExpressionTime, -900, -120), spray, "Time")
    vertex_color = expression(unreal.MaterialExpressionVertexColor, -1100, -40)
    rock = expression(unreal.MaterialExpressionComponentMask, -900, -40)
    rock.set_editor_property("r", True)
    rock.set_editor_property("g", False)
    rock.set_editor_property("b", False)
    rock.set_editor_property("a", False)
    connect(vertex_color, rock, "")
    connect(rock, spray, "Rock")
    shore_map = expression(unreal.MaterialExpressionTextureObjectParameter, -900, 40)
    shore_map.set_editor_property("parameter_name", "ShoreMap")
    shore_map.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/Black.Black"))
    connect(shore_map, spray, "ShoreMap")
    shore_origin = expression(unreal.MaterialExpressionVectorParameter, -900, 140)
    shore_origin.set_editor_property("parameter_name", "ShoreMapOrigin")
    connect(shore_origin, spray, "ShoreMapOrigin")
    connect(scalar("ShoreMapSize", 0.0, -900, 220), spray, "ShoreMapSize")
    connect(scalar("ShoreSwellHeight", SHORE_SWELL_HEIGHT_M, -900, 300), spray, "SwellHeight")
    connect(scalar("ShoreSwellPeriod", SHORE_SWELL_PERIOD_S, -900, 380), spray, "SwellPeriod")
    connect(expression(unreal.MaterialExpressionSkyAtmosphereLightDirection, -900, 460), spray, "ToSun")
    connect(expression(unreal.MaterialExpressionCameraVectorWS, -900, 540), spray, "ToCamera")
    connect(expression(unreal.MaterialExpressionVertexNormalWS, -900, 620), spray, "Shoreward")

    # Light: radiance relative to the sun's illuminance, a faintly blue-white mist.
    lit = expression(unreal.MaterialExpressionMultiply, -250, 200)
    connect(spray, lit, "A", "Radiance")
    connect(expression(unreal.MaterialExpressionSkyAtmosphereLightIlluminance, -500, 300), lit, "B")
    tint = expression(unreal.MaterialExpressionVectorParameter, -500, 380)
    tint.set_editor_property("parameter_name", "SprayColor")
    tint.set_editor_property("default_value", unreal.LinearColor(0.95, 0.97, 1.0, 1.0))
    emissive = expression(unreal.MaterialExpressionMultiply, -100, 250)
    connect(lit, emissive, "A")
    connect(tint, emissive, "B")
    MEL.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    # Soft where the curtain meets the water and the rocks.
    fade = expression(unreal.MaterialExpressionDepthFade, -200, 0)
    fade.set_editor_property("fade_distance_default", 60.0)
    connect(spray, fade, "Opacity")
    strength = scalar("SprayStrength", 1.0, -350, 80)
    opacity = expression(unreal.MaterialExpressionMultiply, -60, 0)
    connect(fade, opacity, "A")
    connect(strength, opacity, "B")
    MEL.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(SPRAY_MATERIAL_PATH)


def seabed_material():
    """Bright, slightly warm coral sand: it is what turns the clear water turquoise."""
    if unreal.EditorAssetLibrary.does_asset_exist(SEABED_MATERIAL_PATH):
        material = unreal.EditorAssetLibrary.load_asset(SEABED_MATERIAL_PATH)
        MEL.delete_all_material_expressions(material)
    else:
        directory, name = SEABED_MATERIAL_PATH.rsplit("/", 1)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, directory, unreal.Material, unreal.MaterialFactoryNew())

    def expression(cls, x, y):
        return MEL.create_material_expression(material, cls, x, y)

    def scalar(name, value, x, y):
        node = expression(unreal.MaterialExpressionScalarParameter, x, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", value)
        return node

    def connect(source, target, input_name):
        if not MEL.connect_material_expressions(source, "", target, input_name):
            raise RuntimeError(f"Could not connect {source.get_name()} -> {target.get_name()}.{input_name}")

    sand = expression(unreal.MaterialExpressionVectorParameter, -400, 0)
    sand.set_editor_property("parameter_name", "SandColor")
    # Mean linear albedo of the grain mix (OceanSandGrains): white-cream carbonate sand. Local exposure keeps
    # the sunlit beach from blowing out.
    sand.set_editor_property("default_value", unreal.LinearColor(0.66, 0.61, 0.51, 1.0))

    material.set_editor_property("tangent_space_normal", False)  # the surface function returns world normals

    world_pos = expression(unreal.MaterialExpressionWorldPosition, -1000, 300)
    time = expression(unreal.MaterialExpressionTime, -1000, 400)
    vertex_normal = expression(unreal.MaterialExpressionVertexNormalWS, -1000, 200)

    # Sand, wet sand, rock, vegetation, coral and seagrass (OceanTerrainSurface in Shaders/Ocean.ush).
    # ShoreSwellHeight must match the ocean's (FOceanSettings::ShoreSwellHeight) for the wet band.
    swell_height = scalar("ShoreSwellHeight", SHORE_SWELL_HEIGHT_M, -1000, -200)
    surface = expression(unreal.MaterialExpressionCustom, -700, -100)
    surface.set_editor_property("description", "Terrain Surface")
    surface.set_editor_property(
        "code", "return OceanTerrainSurface(WorldPos, VertexNormal, Time, SwellHeight, SandColor, ToSun, ToCamera, "
        "ShoreMap, ShoreMapSampler, ShoreMapOrigin, ShoreMapSize, SwellPeriod, SwellMaxDepth, "
        "RockAlbedo, GS, RockNormal, GS, SoilAlbedo, GS, SoilNormal, GS, "
        "LitterAlbedo, GS, LitterNormal, GS, GrassAlbedo, GS, GrassNormal, GS, "
        "WrackAlbedo, GS, WrackNormal, GS, "
        "RockTint, SoilTint, LitterTint, GrassTint, Deform, DeformSampler, DeformWindow, Roughness, Normal, Glint);")
    # The ground textures share the material's wrap sampler (GS): with one sampler each the pixel shader exceeded its
    # 16 sampler registers (the per-texture samplers are then unused and compiled out).
    gs_define = unreal.CustomDefine()
    gs_define.set_editor_property("define_name", "GS")
    gs_define.set_editor_property("define_value", "Material.Wrap_WorldGroupSettings")
    surface.set_editor_property("additional_defines", [gs_define])
    surface.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    surface.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    surface_inputs = []
    for name in ("WorldPos", "VertexNormal", "Time", "SwellHeight", "SandColor", "ToSun", "ToCamera",
                 "ShoreMap", "ShoreMapOrigin", "ShoreMapSize", "SwellPeriod", "SwellMaxDepth",
                 "RockAlbedo", "RockNormal", "SoilAlbedo", "SoilNormal", "LitterAlbedo", "LitterNormal", "GrassAlbedo", "GrassNormal",
                 "WrackAlbedo", "WrackNormal",
                 "RockTint", "SoilTint", "LitterTint", "GrassTint", "Deform", "DeformWindow"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        surface_inputs.append(custom_input)
    surface.set_editor_property("inputs", surface_inputs)
    outputs = []
    for name, kind in (("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
                       ("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
                       ("Glint", unreal.CustomMaterialOutputType.CMOT_FLOAT1)):
        custom_output = unreal.CustomOutput()
        custom_output.set_editor_property("output_name", name)
        custom_output.set_editor_property("output_type", kind)
        outputs.append(custom_output)
    surface.set_editor_property("additional_outputs", outputs)
    connect(world_pos, surface, "WorldPos")
    connect(vertex_normal, surface, "VertexNormal")
    connect(time, surface, "Time")
    connect(swell_height, surface, "SwellHeight")
    connect(sand, surface, "SandColor")
    # Sand sparkles need the sun and view directions.
    to_sun = expression(unreal.MaterialExpressionVectorParameter, -1000, 600)
    to_sun.set_editor_property("parameter_name", "ToSun")
    to_sun.set_editor_property("default_value", unreal.LinearColor(*sun_direction(), 0.0))
    connect(to_sun, surface, "ToSun")
    connect(expression(unreal.MaterialExpressionCameraVectorWS, -1000, 700), surface, "ToCamera")
    # Coastal data (AOceanBathymetry, set on its dynamic instance) and the shore swell, for the wave ripples.
    shore_map = expression(unreal.MaterialExpressionTextureObjectParameter, -1300, 1000)
    shore_map.set_editor_property("parameter_name", "ShoreMap")
    shore_map.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/Black.Black"))
    connect(shore_map, surface, "ShoreMap")
    shore_origin = expression(unreal.MaterialExpressionVectorParameter, -1300, 1100)
    shore_origin.set_editor_property("parameter_name", "ShoreMapOrigin")
    connect(shore_origin, surface, "ShoreMapOrigin")
    connect(scalar("ShoreMapSize", 0.0, -1300, 1200), surface, "ShoreMapSize")
    connect(scalar("ShoreSwellPeriod", SHORE_SWELL_PERIOD_S, -1300, 1280), surface, "SwellPeriod")
    connect(scalar("ShoreSwellMaxDepth", SHORE_SWELL_MAX_DEPTH_M, -1300, 1360), surface, "SwellMaxDepth")
    # Ground textures rendered from Infinigen's terrain materials (Scripts/infinigen/render_ground_tiles.py), and the
    # tints that bring them to Caribbean colours: pale grey limestone, brown-red soil, tan dry leaves.
    ground = import_ground_textures()
    for index, (kind, prefix) in enumerate((("limestone", "Rock"), ("soil", "Soil"), ("litter", "Litter"), ("grass", "Grass"),
                                            ("wrack", "Wrack"))):
        for row, (suffix, input_suffix) in enumerate((("BaseColor", "Albedo"), ("Normal", "Normal"))):
            texture = expression(unreal.MaterialExpressionTextureObjectParameter, -1600, 1500 + 200 * (2 * index + row))
            texture.set_editor_property("parameter_name", f"Ground{prefix}{input_suffix}")
            texture.set_editor_property("texture", ground.get((kind, suffix)) or unreal.load_asset(
                "/Engine/EngineResources/Black.Black" if suffix == "BaseColor" else "/Engine/EngineMaterials/FlatNormal.FlatNormal"))
            # Shared wrap sampler: the material would otherwise exceed the 16 samplers of a pixel shader.
            texture.set_editor_property("sampler_source", unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
            if suffix == "Normal":
                texture.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
            connect(texture, surface, f"{prefix}{input_suffix}")
    for index, (prefix, tint) in enumerate((("Rock", GROUND_ROCK_TINT), ("Soil", GROUND_SOIL_TINT), ("Litter", GROUND_LITTER_TINT),
                                            ("Grass", GROUND_GRASS_TINT))):
        node = expression(unreal.MaterialExpressionVectorParameter, -1600, 3200 + 100 * index)
        node.set_editor_property("parameter_name", f"Ground{prefix}Tint")
        node.set_editor_property("default_value", unreal.LinearColor(*tint, 1.0))
        connect(node, surface, f"{prefix}Tint")
    # The sand the player changed (UOceanSandDeformation, set on the dynamic instance by AOceanBathymetry): footprints,
    # holes. Window = lower corner and size, m; size 0 = nothing changed.
    deform = expression(unreal.MaterialExpressionTextureObjectParameter, -1300, 1440)
    deform.set_editor_property("parameter_name", "SandDeformation")
    deform.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/Black.Black"))
    connect(deform, surface, "Deform")
    deform_window = expression(unreal.MaterialExpressionVectorParameter, -1300, 1520)
    deform_window.set_editor_property("parameter_name", "SandDeformationWindow")
    deform_window.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    connect(deform_window, surface, "DeformWindow")
    MEL.connect_material_property(surface, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(surface, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(surface, "Normal", unreal.MaterialProperty.MP_NORMAL)

    # Caustics (Shaders/Ocean.ush): sunlight focused by the simulated waves, from the curvature of the two finer
    # cascades. AOceanActor sets their slope textures (Deriv1, Deriv2) on the terrain's material instance.
    fade_depth = scalar("CausticFadeDepth", 12.0, -1000, 560)
    placeholder = unreal.load_asset("/Engine/EngineResources/Black.Black")
    derivs = []
    for index, y in ((1, 1500), (2, 1600)):
        deriv = expression(unreal.MaterialExpressionTextureObjectParameter, -1300, y)
        deriv.set_editor_property("parameter_name", f"Deriv{index}")
        deriv.set_editor_property("texture", placeholder)
        derivs.append(deriv)
    caustics = expression(unreal.MaterialExpressionCustom, -700, 400)
    caustics.set_editor_property("description", "Ocean Caustics")
    caustics.set_editor_property(
        "code", "return OceanCaustics(WorldPos, ToSun, Deriv1, Deriv1Sampler, Deriv2, Deriv2Sampler, "
                "PatchSize1, PatchSize2, FadeDepth);")
    caustics.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    caustics.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    inputs = []
    for name in ("WorldPos", "ToSun", "Deriv1", "Deriv2", "PatchSize1", "PatchSize2", "FadeDepth"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    caustics.set_editor_property("inputs", inputs)
    connect(world_pos, caustics, "WorldPos")
    connect(to_sun, caustics, "ToSun")
    connect(derivs[0], caustics, "Deriv1")
    connect(derivs[1], caustics, "Deriv2")
    connect(scalar("PatchSize1", 0.0, -1300, 1700), caustics, "PatchSize1")  # 0 until the ocean sets them
    connect(scalar("PatchSize2", 0.0, -1300, 1780), caustics, "PatchSize2")
    connect(fade_depth, caustics, "FadeDepth")

    # Emissive in the same range as the sunlit sand (~albedo * sun lux / pi), times the focus above the mean.
    strength = scalar("CausticStrength", 1.0, -700, 560)
    scaled = expression(unreal.MaterialExpressionMultiply, -450, 450)
    connect(caustics, scaled, "A")
    connect(strength, scaled, "B")
    tinted = expression(unreal.MaterialExpressionMultiply, -250, 400)
    connect(surface, tinted, "A")  # caustics light whatever the floor is: sand, coral, seagrass
    connect(scaled, tinted, "B")
    # Sparkles, in the same emissive units: sun radiance ~ sun lux / pi, warm sun colour.
    glint_strength = scalar("GlintStrength", 60.0, -450, 650)
    glint = expression(unreal.MaterialExpressionMultiply, -250, 600)
    MEL.connect_material_expressions(surface, "Glint", glint, "A")
    connect(glint_strength, glint, "B")
    emissive = expression(unreal.MaterialExpressionAdd, -100, 500)
    connect(tinted, emissive, "A")
    connect(glint, emissive, "B")
    MEL.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(SEABED_MATERIAL_PATH)
    return material


def caribbean_sea_state(ocean):
    """A fine trade-wind day: a moderate breeze (Beaufort 4) raising a lively chop with scattered whitecaps,
    and a low swell lapping on the beaches."""
    settings = ocean.get_editor_property("settings")
    settings.set_editor_property("wind_speed", 7.0)
    settings.set_editor_property("wind_direction", 200.0)
    settings.set_editor_property("fetch", 60000.0)
    settings.set_editor_property("swell", 0.3)
    settings.set_editor_property("choppiness", 0.9)
    # Beaufort 4: whitecaps here and there, not everywhere.
    settings.set_editor_property("foam_threshold", 0.87)
    settings.set_editor_property("foam_intensity", 1.5)
    settings.set_editor_property("foam_decay", 0.25)
    settings.set_editor_property("shore_swell_height", SHORE_SWELL_HEIGHT_M)
    settings.set_editor_property("shore_swell_period", SHORE_SWELL_PERIOD_S)
    settings.set_editor_property("shore_swell_max_depth", SHORE_SWELL_MAX_DEPTH_M)
    settings.set_editor_property("resolution", 256)
    # The finest cascade (10 m) on the GPU: ripples down to ~1 cm, sharp caustics (0 = on the CPU at 256).
    settings.set_editor_property("gpu_detail_resolution", GPU_DETAIL_RESOLUTION)
    ocean.set_editor_property("settings", settings)


def build():
    # The map may be the editor startup map and already loaded, so empty it instead of deleting it.
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        if not levels.load_level(MAP_PATH):
            raise RuntimeError(f"Could not load level {MAP_PATH}")
        for actor in actors.get_all_level_actors():
            if not isinstance(actor, unreal.WorldSettings):
                actors.destroy_actor(actor)
    elif not levels.new_level(MAP_PATH):
        raise RuntimeError(f"Could not create level {MAP_PATH}")

    # Free-flying camera, no playable ship: GameModeBase's default pawn is ADefaultPawn.
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.GameModeBase.static_class())

    # --- Sky: late afternoon sun (SUN_ELEVATION_DEG) in a physical atmosphere, real-time sky light, cumulus ---
    # From the side of the usual views: the relief, the palms and the waves are modelled by light and shade, with long
    # shadows across the beach, rather than front-lit and flat like a midday holiday photo.
    sun = spawn(unreal.DirectionalLight, (0.0, 0.0, 1000.0), SUN_ROTATION, "Sun")
    sun_light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun_light.set_editor_property("atmosphere_sun_light", True)
    sun_light.set_editor_property("intensity", 10.0)
    sun_light.set_editor_property("light_color", unreal.Color(r=255, g=248, b=235, a=255))

    # Clean maritime tropical air: the default Mie (aerosol) scattering is that of a hazy continental sky,
    # which turns the whole horizon milky white. A third of it keeps a soft horizon glow with a deep blue sky.
    atmosphere = spawn(unreal.SkyAtmosphere, label="SkyAtmosphere")
    atmosphere_component = atmosphere.get_component_by_class(unreal.SkyAtmosphereComponent)
    atmosphere_component.set_editor_property("mie_scattering_scale", SKY_MIE_SCATTERING)
    atmosphere_component.set_editor_property("mie_absorption_scale", SKY_MIE_ABSORPTION)

    sky = spawn(unreal.SkyLight, (0.0, 0.0, 500.0), label="SkyLight")
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.set_editor_property("real_time_capture", True)

    spawn(unreal.VolumetricCloud, label="Clouds")

    # Exposure: the auto exposure meters a frame dominated by dark water (it reflects a few % of the light),
    # so it overexposes everything brighter, and the sunlit beach clips to white. Local exposure compresses
    # the highlights like a good camera without darkening the water; less bloom removes the halo.
    post = spawn(unreal.PostProcessVolume, label="PostProcess")
    post.set_editor_property("unbound", True)
    settings = post.get_editor_property("settings")
    for name, value in POST_PROCESS.items():
        settings.set_editor_property(f"override_{name}", True)
        settings.set_editor_property(name, value)
    post.set_editor_property("settings", settings)

    # The sky atmosphere already gives the distance its aerial perspective; the height fog only adds a faint haze.
    fog = spawn(unreal.ExponentialHeightFog, label="Fog")
    fog.get_component_by_class(unreal.ExponentialHeightFogComponent).set_editor_property("fog_density", SKY_FOG_DENSITY)

    # --- Ocean: its Z is the sea level ---
    ocean = spawn(load_class("/Script/Makar.OceanActor"), (0.0, 0.0, 0.0), label="Ocean")
    caribbean_sea_state(ocean)

    # --- Island, beach, shallow shelf and reef drop-off (AOceanBathymetry). Its coastal data also calms
    # the waves in shallow water, for rendering and water queries alike. Must sit at Z = 0. ---
    seabed_material()  # the terrain's material (sand + caustics)
    spray_material()   # the spray along the line where the swell breaks
    island = spawn(load_class("/Script/Makar.OceanBathymetry"), (ISLAND_CENTRE_M[0] * 100.0, ISLAND_CENTRE_M[1] * 100.0, 0.0),
                   label="Island")
    island.set_editor_property("island_radius", ISLAND_RADIUS_M)
    island.set_editor_property("rockiness", 0.85)  # dark rocky points between the beaches

    # --- Caribbean flora and limestone boulders, generated on load (AOceanIslandFlora, needs
    # create_flora_materials.py) ---
    spawn(load_class("/Script/Makar.OceanIslandFlora"), (ISLAND_CENTRE_M[0] * 100.0, ISLAND_CENTRE_M[1] * 100.0, 0.0),
          label="IslandFlora")

    # --- Camera start: on the shelf a few metres above the water, facing the beach; the trade-wind
    # waves roll in from behind towards the island ---
    spawn(unreal.PlayerStart, (0.0, 0.0, 400.0), (0.0, 0.0, 180.0), label="PlayerStart")

    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {MAP_PATH}")
    unreal.log(f"Ocean map written to {MAP_PATH}")


build()
