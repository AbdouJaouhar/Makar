"""
Builds /Game/Maps/Ocean: a Caribbean sea showcase (clear water over a white sand shelf, tropical sun,
trade-wind sea state) explored with a free-flying camera.

Run after create_ocean_material.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_ocean_map.py
Running it again rebuilds the level from scratch.
"""

import unreal

MAP_PATH = "/Game/Maps/Ocean"
SEABED_MATERIAL_PATH = "/Game/Ocean/M_Seabed"

# Seabed: a gently sloping sand shelf, shallow near the start and deepening offshore (+X).
SEABED_SIZE_M = 8000.0
SEABED_DEPTH_AT_START_M = 8.0
SEABED_SLOPE_DEG = 0.08  # ~1.4 m deeper every kilometre: 2.4 m behind the start, 13.6 m at the far edge

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


def seabed_material():
    """Bright, slightly warm coral sand: it is what turns the clear water turquoise."""
    if unreal.EditorAssetLibrary.does_asset_exist(SEABED_MATERIAL_PATH):
        material = unreal.EditorAssetLibrary.load_asset(SEABED_MATERIAL_PATH)
        MEL.delete_all_material_expressions(material)
    else:
        directory, name = SEABED_MATERIAL_PATH.rsplit("/", 1)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, directory, unreal.Material, unreal.MaterialFactoryNew())

    sand = MEL.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -400, 0)
    sand.set_editor_property("parameter_name", "SandColor")
    sand.set_editor_property("default_value", unreal.LinearColor(0.78, 0.7, 0.52, 1.0))
    MEL.connect_material_property(sand, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = MEL.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 200)
    roughness.set_editor_property("r", 0.95)
    MEL.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(SEABED_MATERIAL_PATH)
    return material


def caribbean_sea_state(ocean):
    """Trade winds over the open Atlantic: steady moderate breeze, long fetch, few whitecaps."""
    settings = ocean.get_editor_property("settings")
    settings.set_editor_property("wind_speed", 7.0)
    settings.set_editor_property("wind_direction", 200.0)
    settings.set_editor_property("fetch", 300000.0)
    settings.set_editor_property("swell", 0.35)
    settings.set_editor_property("choppiness", 0.9)
    settings.set_editor_property("foam_threshold", 0.5)
    settings.set_editor_property("foam_intensity", 1.5)
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
    sun = spawn(unreal.DirectionalLight, (0.0, 0.0, 1000.0), (0.0, -50.0, 35.0), "Sun")
    sun_light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun_light.set_editor_property("atmosphere_sun_light", True)
    sun_light.set_editor_property("intensity", 10.0)
    sun_light.set_editor_property("light_color", unreal.Color(r=255, g=248, b=235, a=255))

    spawn(unreal.SkyAtmosphere, label="SkyAtmosphere")

    sky = spawn(unreal.SkyLight, (0.0, 0.0, 500.0), label="SkyLight")
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.set_editor_property("real_time_capture", True)

    spawn(unreal.VolumetricCloud, label="Clouds")

    # Clean tropical air: only a light haze on the horizon.
    fog = spawn(unreal.ExponentialHeightFog, label="Fog")
    fog.get_component_by_class(unreal.ExponentialHeightFogComponent).set_editor_property("fog_density", 0.002)

    # --- Ocean: its Z is the sea level ---
    ocean = spawn(load_class("/Script/Makar.OceanActor"), (0.0, 0.0, 0.0), label="Ocean")
    caribbean_sea_state(ocean)

    # --- Sand shelf seen through the water ---
    seabed = spawn(unreal.StaticMeshActor, (0.0, 0.0, -SEABED_DEPTH_AT_START_M * 100.0),
                   (0.0, -SEABED_SLOPE_DEG, 0.0), "Seabed")
    seabed_mesh = seabed.static_mesh_component
    seabed_mesh.set_mobility(unreal.ComponentMobility.MOVABLE)  # lit by Lumen, no baked lighting
    seabed_mesh.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Plane.Plane"))
    seabed_mesh.set_material(0, seabed_material())
    seabed.set_actor_scale3d(unreal.Vector(SEABED_SIZE_M, SEABED_SIZE_M, 1.0))  # plane is 1 m wide

    # --- Camera start: a few metres above the water, looking offshore ---
    spawn(unreal.PlayerStart, (0.0, 0.0, 400.0), label="PlayerStart")

    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {MAP_PATH}")
    unreal.log(f"Ocean map written to {MAP_PATH}")


build()
