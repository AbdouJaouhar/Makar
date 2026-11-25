"""
Builds /Game/Maps/Shipyard: the ship viewer. One procedural ship (AProceduralShip) in a photo studio, nothing else: no
sun, no sky, no ground; a dark graded backdrop, three studio lights (key, fill, rim) and the backdrop's soft light.
The game mode (AShipViewerGameMode) orbits the camera around the ship and shows the options menu.

Run after create_ship_materials.py, from the editor (Tools > Execute Python Script...) or headless:
    UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/create_shipyard_map.py
Running it again rebuilds the level from scratch.
"""

import unreal

MAP_PATH = "/Game/Maps/Shipyard"
DIRECTORY = "/Game/Ship"
MEL = unreal.MaterialEditingLibrary

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


def backdrop_material():
    """A studio's seamless backdrop seen all around: near black below, a soft grey glow above the horizon. The sky
    light captures it: the studio's soft ambient light comes from it."""
    name = "M_StudioBackdrop"
    path = f"{DIRECTORY}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DIRECTORY, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    camera = MEL.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS, -700, 0)
    shade = MEL.create_material_expression(material, unreal.MaterialExpressionCustom, -400, 0)
    shade.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    shade.set_editor_property("code",
                              "float Up = -Camera.z;"
                              " float Glow = smoothstep(-0.7, 0.8, Up);"
                              " return lerp(float3(0.018, 0.019, 0.021), float3(0.2, 0.205, 0.215), Glow * Glow);")
    pin = unreal.CustomInput()
    pin.set_editor_property("input_name", "Camera")
    shade.set_editor_property("inputs", [pin])
    MEL.connect_material_expressions(camera, "", shade, "Camera")
    MEL.connect_material_property(shade, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    return material


def light(label, pitch, yaw, intensity, colour, shadows):
    actor = spawn(unreal.DirectionalLight, (0.0, 0.0, 2000.0), (0.0, pitch, yaw), label)
    component = actor.get_component_by_class(unreal.DirectionalLightComponent)
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    component.set_editor_property("intensity", intensity)
    component.set_editor_property("light_color", unreal.Color(r=colour[0], g=colour[1], b=colour[2], a=255))
    component.set_editor_property("cast_shadows", shadows)
    component.set_editor_property("light_source_angle", 6.0)  # a large softbox: soft-edged shadows
    component.set_editor_property("atmosphere_sun_light", False)
    component.set_editor_property("forward_shading_priority", 1 if shadows else 0)
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
    world.get_world_settings().set_editor_property("default_game_mode", load_class("/Script/Makar.ShipViewerGameMode"))

    # Backdrop: a sphere 1 km across around the ship.
    backdrop = spawn(unreal.StaticMeshActor, (0.0, 0.0, 0.0), label="Backdrop")
    mesh = backdrop.static_mesh_component
    mesh.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Sphere"))
    mesh.set_material(0, backdrop_material())
    mesh.set_editor_property("cast_shadow", False)
    mesh.set_collision_profile_name("NoCollision")  # a backdrop: the visitor inside it must not be pushed out
    mesh.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    backdrop.set_actor_scale3d(unreal.Vector(2000.0, 2000.0, 2000.0))

    # Three-point lighting: a warm key high on the port bow, a cool dim fill from the other side, a rim from behind.
    light("Key", -38.0, 40.0, 7.0, (255, 246, 232), True)
    light("Fill", -12.0, -130.0, 1.2, (214, 226, 255), False)
    light("Rim", -22.0, 195.0, 4.0, (255, 250, 245), False)
    sky = spawn(unreal.SkyLight, (0.0, 0.0, 500.0), label="StudioAmbient")
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    # Captures the backdrop (beyond its distance threshold) at load: the studio's soft ambient light.
    sky_light.set_editor_property("real_time_capture", False)
    sky_light.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_CAPTURED_SCENE)
    sky_light.set_editor_property("sky_distance_threshold", 50000.0)
    sky_light.set_editor_property("intensity", 1.5)

    # A fixed exposure (the camera turning around the ship must not pump), a touch of vignette.
    post = spawn(unreal.PostProcessVolume, label="PostProcess")
    post.set_editor_property("unbound", True)
    settings = post.get_editor_property("settings")
    for name, value in (("auto_exposure_method", unreal.AutoExposureMethod.AEM_MANUAL),
                        ("auto_exposure_apply_physical_camera_exposure", False),
                        ("auto_exposure_bias", 0.0),
                        ("vignette_intensity", 0.5),
                        ("bloom_intensity", 0.3)):
        settings.set_editor_property(f"override_{name}", True)
        settings.set_editor_property(name, value)
    post.set_editor_property("settings", settings)

    ship = spawn(load_class("/Script/Makar.ProceduralShip"), (0.0, 0.0, 0.0), label="Ship")
    ship.set_editor_property("type", unreal.ShipType.FRIGATE)
    ship.set_editor_property("seed", 1)
    ship.set_editor_property("on_cradle", False)
    ship.rebuild()

    spawn(unreal.PlayerStart, (-6000.0, -6000.0, 1200.0), (0.0, -6.0, 45.0), label="PlayerStart")

    if not levels.save_current_level():
        raise RuntimeError(f"Could not save {MAP_PATH}")
    unreal.log(f"Shipyard map written to {MAP_PATH}")


build()
