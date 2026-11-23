"""Bakes the island's flora into Nanite static meshes (AOceanIslandFlora::BakeMeshes).

Every palm, tree, shrub, small plant, fallen frond, coconut, grass tuft and stone is generated once here and saved
in /Game/Ocean/Flora/Baked as a Nanite mesh (foliage voxelized in the distance); the flora then loads them instead of
generating and building them on every load, and Nanite draws their millions of triangles. Re-run after changing a
generator (and bump OceanFlora::GeneratorVersion, so stale meshes are not loaded).

Run: UnrealEditor-Cmd.exe Makar.uproject -run=pythonscript -script=Scripts/bake_flora.py -unattended
"""
import unreal

MAP = "/Game/Maps/Ocean"

unreal.EditorLoadingAndSavingUtils.load_map(MAP)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
flora_class = unreal.load_class(None, "/Script/Makar.OceanIslandFlora")
floras = [a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
          if a.get_class() == flora_class]
if not floras:
    unreal.log_error("bake_flora: no OceanIslandFlora in " + MAP)
for flora in floras:
    flora.bake_meshes()
    unreal.log(f"bake_flora: baked {flora.get_actor_label()}")
