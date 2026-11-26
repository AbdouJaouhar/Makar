"""Sail success criteria over many ships: every type, a few seeds, eight winds (needs MAKAR_VERIFY_SAILS=1)."""
import os
import time

import unreal

TYPES = [t for t in os.environ.get("SAILCHECK_TYPES", "SLOOP,BRIGANTINE,FRIGATE,GALLEON").split(",") if t]
SEEDS = [int(s) for s in os.environ.get("SAILCHECK_SEEDS", "1,2").split(",") if s]
WINDS = [float(w) for w in os.environ.get("SAILCHECK_WINDS", "0,45,90,135,180,225,270,315").split(",") if w]
state = {"start": None, "done": False}


def run():
    cls = unreal.load_class(None, "/Script/Makar.ProceduralShip")
    ship = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(cls, unreal.Vector(0, 0, 0))
    ship.set_editor_property("on_cradle", False)
    for t in TYPES:
        for seed in SEEDS:
            for wind in WINDS:
                ship.set_editor_property("type", getattr(unreal.ShipType, t))
                ship.set_editor_property("seed", seed)
                ship.set_editor_property("wind_from", wind)
                ship.rebuild()
    unreal.log("SAILCHECK finished")


def tick(dt):
    try:
        now = time.time()
        if state["start"] is None:
            state["start"] = now
            return
        if not state["done"] and now - state["start"] > 20.0:
            state["done"] = True
            run()
            unreal.SystemLibrary.quit_editor()
    except Exception as error:
        unreal.log_error(f"SAILCHECK failed: {error}")
        unreal.SystemLibrary.quit_editor()


unreal.register_slate_post_tick_callback(tick)
