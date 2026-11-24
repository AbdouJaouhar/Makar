"""
Builds /Game/Maps/Shipyard: the procedural ships (AProceduralShip) side by side on their building berths, one of each
type, on a neutral grey ground under an early afternoon sun, to judge their shapes and materials in the clear.

Run after create_ship_materials.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_shipyard_map.py
Running it again rebuilds the level from scratch. MAKAR_SHIP_SEED overrides the ships' seed.
"""

import os

import unreal

MAP_PATH = "/Game/Maps/Shipyard"
SEED = int(os.environ.get("MAKAR_SHIP_SEED", "1"))

# The ships in a row along Y, bows towards +X: (type, Y in m).
SHIPS = (("Sloop", 0.0), ("Brigantine", 90.0), ("Frigate", 190.0), ("Galleon", 300.0))

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


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


def build():
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        if not levels.load_level(MAP_PATH):
            raise RuntimeError(f"Could not load level {MAP_PATH}")
        for actor in actors.get_all_level_actors():
            if not isinstance(actor, unreal.WorldSettings):
                actors.destroy_actor(actor)
    elif not levels.new_level(MAP_PATH):
        raise RuntimeError(f"Could not create level {MAP_PATH}")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", load_class("/Script/Makar.MakarViewerGameMode"))

    # Sun at 45 degrees from the south-west of the row (the ships' port bows lit), physical sky, real-time sky light.
    sun = spawn(unreal.DirectionalLight, (0.0, 0.0, 1000.0), (0.0, -45.0, 35.0), "Sun")
    light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    light.set_mobility(unreal.ComponentMobility.MOVABLE)
    light.set_editor_property("atmosphere_sun_light", True)
    light.set_editor_property("intensity", 10.0)
    light.set_editor_property("light_color", unreal.Color(r=255, g=248, b=238, a=255))
    atmosphere = spawn(unreal.SkyAtmosphere, label="SkyAtmosphere")
    component = atmosphere.get_component_by_class(unreal.SkyAtmosphereComponent)
    component.set_editor_property("mie_scattering_scale", 0.0013)
    component.set_editor_property("mie_absorption_scale", 0.00015)
    sky = spawn(unreal.SkyLight, (0.0, 0.0, 500.0), label="SkyLight")
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.set_editor_property("real_time_capture", True)
    spawn(unreal.VolumetricCloud, label="Clouds")
    fog = spawn(unreal.ExponentialHeightFog, label="Fog")
    fog.get_component_by_class(unreal.ExponentialHeightFogComponent).set_editor_property("fog_density", 0.0004)
    post = spawn(unreal.PostProcessVolume, label="PostProcess")
    post.set_editor_property("unbound", True)
    settings = post.get_editor_property("settings")
    for name, value in (("local_exposure_highlight_contrast_scale", 0.75), ("local_exposure_shadow_contrast_scale", 0.85),
                        ("bloom_intensity", 0.4), ("auto_exposure_bias", -0.2)):
        settings.set_editor_property(f"override_{name}", True)
        settings.set_editor_property(name, value)
    post.set_editor_property("settings", settings)

    # Ground: a 2 km grey plane.
    ground = spawn(unreal.StaticMeshActor, (0.0, 150.0 * 100.0, 0.0), label="Ground")
    mesh = ground.static_mesh_component
    mesh.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Plane"))
    mesh.set_material(0, unreal.load_asset("/Game/Ship/M_ShipyardFloor"))
    ground.set_actor_scale3d(unreal.Vector(2000.0, 2000.0, 1.0))

    ship_class = load_class("/Script/Makar.ProceduralShip")
    for type_name, y in SHIPS:
        ship = spawn(ship_class, (0.0, y * 100.0, 0.0), label=f"Ship_{type_name}")
        ship.set_editor_property("type", getattr(unreal.ShipType, type_name.upper()))
        ship.set_editor_property("seed", SEED)
        ship.set_editor_property("on_cradle", True)
        ship.rebuild()

    spawn(unreal.PlayerStart, (-5000.0, -6000.0, 1200.0), (0.0, -6.0, 50.0), label="PlayerStart")

    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {MAP_PATH}")
    unreal.log(f"Shipyard map written to {MAP_PATH}")


build()
