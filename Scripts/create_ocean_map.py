"""
Builds /Game/Maps/Ocean: a Caribbean sea showcase (a sandy island, clear water over a white sand shelf,
a reef drop-off, tropical sun, trade-wind sea state) explored with a free-flying camera.

Run after create_ocean_material.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_ocean_map.py
Running it again rebuilds the level from scratch.
"""

import math

import unreal

MAP_PATH = "/Game/Maps/Ocean"
SEABED_MATERIAL_PATH = "/Game/Ocean/M_Seabed"

# Island (AOceanBathymetry) west of the start: its beach faces the trade-wind waves, which travel towards -X.
# The shelf around it is ~7 m deep and drops into deep water ~450 m from the shore (behind the start).
ISLAND_CENTRE_M = (-500.0, 0.0)
ISLAND_RADIUS_M = 250.0

# Sky: Mie (aerosol) scattering and absorption scales (engine defaults 0.003996 / 0.000444), height fog density.
SKY_MIE_SCATTERING = 0.0013
SKY_MIE_ABSORPTION = 0.00015
SKY_FOG_DENSITY = 0.0003

# Swell lapping on the beaches of the lagoon (ocean settings and the wet sand band of M_Seabed).
SHORE_SWELL_HEIGHT_M = 0.4

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


SUN_ROTATION = (0.0, -50.0, 215.0)  # roll, pitch, yaw of the directional light


def sun_direction():
    """Unit vector towards the sun (the light travels along the actor's forward axis)."""
    _, pitch, yaw = (math.radians(a) for a in SUN_ROTATION)
    return (-math.cos(pitch) * math.cos(yaw), -math.cos(pitch) * math.sin(yaw), -math.sin(pitch))


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
        "code", "return OceanTerrainSurface(WorldPos, VertexNormal, Time, SwellHeight, SandColor, ToSun, ToCamera, Footprints, FootprintsSampler, FootprintTileSize, Roughness, Normal, Glint);")
    surface.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    surface.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    surface_inputs = []
    for name in ("WorldPos", "VertexNormal", "Time", "SwellHeight", "SandColor", "ToSun", "ToCamera", "Footprints", "FootprintTileSize"):
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
    # Footprints simulated by AOceanBathymetry (set on its dynamic instance); black = untouched sand.
    footprints = expression(unreal.MaterialExpressionTextureObjectParameter, -1000, 800)
    footprints.set_editor_property("parameter_name", "SandFootprints")
    footprints.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/Black.Black"))
    connect(footprints, surface, "Footprints")
    connect(scalar("SandFootprintTileSize", 0.0, -1000, 900), surface, "FootprintTileSize")
    MEL.connect_material_property(surface, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(surface, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(surface, "Normal", unreal.MaterialProperty.MP_NORMAL)

    # Caustics (Shaders/Ocean.ush): light focused by the waves, added on top of the sunlit sand.
    cell_size = scalar("CausticCellSize", 1.2, -1000, 480)
    fade_depth = scalar("CausticFadeDepth", 12.0, -1000, 560)
    caustics = expression(unreal.MaterialExpressionCustom, -700, 400)
    caustics.set_editor_property("description", "Ocean Caustics")
    caustics.set_editor_property("code", "return OceanCaustics(WorldPos, Time, CellSize, FadeDepth);")
    caustics.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    caustics.set_editor_property("include_file_paths", ["/Makar/Ocean.ush"])
    inputs = []
    for name in ("WorldPos", "Time", "CellSize", "FadeDepth"):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    caustics.set_editor_property("inputs", inputs)
    connect(world_pos, caustics, "WorldPos")
    connect(time, caustics, "Time")
    connect(cell_size, caustics, "CellSize")
    connect(fade_depth, caustics, "FadeDepth")

    # Emissive in the same range as the sunlit sand (~albedo * sun lux / pi).
    strength = scalar("CausticStrength", 3.0, -700, 560)
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
    """A sheltered lagoon on a fine day: light breeze, glassy water with small ripples, almost no whitecaps,
    and a low swell lapping on the beaches."""
    settings = ocean.get_editor_property("settings")
    settings.set_editor_property("wind_speed", 4.5)
    settings.set_editor_property("wind_direction", 200.0)
    settings.set_editor_property("fetch", 40000.0)
    settings.set_editor_property("swell", 0.3)
    settings.set_editor_property("choppiness", 0.8)
    # The surface barely compresses at this wind speed: whitecaps stay rare.
    settings.set_editor_property("foam_threshold", 0.87)
    settings.set_editor_property("foam_intensity", 1.5)
    settings.set_editor_property("foam_decay", 0.25)
    settings.set_editor_property("shore_swell_height", SHORE_SWELL_HEIGHT_M)
    settings.set_editor_property("shore_swell_period", 7.0)
    settings.set_editor_property("resolution", 256)
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

    # --- Sky: high tropical sun in a physical atmosphere, real-time sky light, scattered cumulus ---
    # The sun is behind the start, facing the island (light travelling towards -X): the island and the lagoon
    # are front-lit like in holiday photos, instead of showing their shaded sides against the light.
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
