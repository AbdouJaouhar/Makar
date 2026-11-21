"""
Builds /Game/Maps/Ocean: the playable test level (ocean, sky, sun, player start, two enemy ships).

Run after create_ocean_material.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_ocean_map.py
Running it again rebuilds the level from scratch.
"""

import unreal

MAP_PATH = "/Game/Maps/Ocean"

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
    # The map may be the editor startup map and already loaded, so empty it instead of deleting it.
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        if not levels.load_level(MAP_PATH):
            raise RuntimeError(f"Could not load level {MAP_PATH}")
        for actor in actors.get_all_level_actors():
            actors.destroy_actor(actor)
    elif not levels.new_level(MAP_PATH):
        raise RuntimeError(f"Could not create level {MAP_PATH}")

    # --- Sky: physical atmosphere lit by a movable sun, real-time captured sky light, clouds, fog ---
    sun = spawn(unreal.DirectionalLight, (0.0, 0.0, 1000.0), (0.0, -25.0, 40.0), "Sun")
    sun_light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun_light.set_editor_property("atmosphere_sun_light", True)
    sun_light.set_editor_property("intensity", 10.0)

    spawn(unreal.SkyAtmosphere, label="SkyAtmosphere")

    sky = spawn(unreal.SkyLight, (0.0, 0.0, 500.0), label="SkyLight")
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.set_editor_property("real_time_capture", True)

    spawn(unreal.VolumetricCloud, label="Clouds")

    fog = spawn(unreal.ExponentialHeightFog, label="Fog")
    fog.get_component_by_class(unreal.ExponentialHeightFogComponent).set_editor_property("fog_density", 0.005)

    # --- Ocean: its Z is the sea level ---
    spawn(load_class("/Script/Makar.OceanActor"), (0.0, 0.0, 0.0), label="Ocean")

    # --- Player ship spawns here (MakarGameMode's default pawn), enemies ahead of it ---
    spawn(unreal.PlayerStart, (0.0, 0.0, 200.0), label="PlayerStart")
    enemy = load_class("/Script/Makar.EnemyShip")
    spawn(enemy, (12000.0, 4000.0, 100.0), (0.0, 0.0, 200.0), "Enemy_1")
    spawn(enemy, (15000.0, -6000.0, 100.0), (0.0, 0.0, 160.0), "Enemy_2")

    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {MAP_PATH}")
    unreal.log(f"Ocean map written to {MAP_PATH}")


build()
