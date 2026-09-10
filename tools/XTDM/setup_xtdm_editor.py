#!/usr/bin/env python3
"""Check/create the two xTDM content Blueprints through a running MapForge editor.

Read-only unless --apply is supplied. Never builds C++, cooks, reparents an
existing asset, or creates a PlayerController/GameState native subclass.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import sys

GS = "/Game/Blueprints/XTDM/BP_NCP_XTDMGameState"
MODE = "/Game/Blueprints/XTDM/NCP_XTDM"
GS_PARENT = "/Script/UnrealTournament.UTGameState"
MODE_PARENT = "/Script/NetcodePlus.NCPlusXTDMGameMode"
CHARACTER = "/Game/Blueprints/Netcode/IGCharacterFootsteps"
RIFLE = "/Game/Blueprints/Netcode/N+InstagibRifle"


def generated(package):
    return package + "." + package.rsplit("/", 1)[1] + "_C"


class Bridge:
    def __init__(self, port):
        self.port = port

    def call(self, cmd, **args):
        request = json.dumps({"id": 1, "cmd": cmd, "args": args})
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.settimeout(60)
            sock.sendall(request.encode("utf-8") + b"\n")
            data = bytearray()
            while not data.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    raise RuntimeError("Editor closed the connection: " + cmd)
                data.extend(chunk)
                if len(data) > 16 * 1024 * 1024:
                    raise RuntimeError("Unexpectedly large editor response")
        response = json.loads(data)
        if not response.get("ok"):
            raise RuntimeError(str(response.get("error", response)))
        return response["result"]

    def dump(self, name):
        return self.call("exec", command="OBJ DUMP " + name).get("output", "")


def verify_parent(bridge, package, parent):
    dump = bridge.dump(package.rsplit("/", 1)[1])
    expected = "Blueprint " + package + "." + package.rsplit("/", 1)[1]
    if expected not in dump or "ParentClass=Class'" + parent + "'" not in dump:
        raise RuntimeError("Unexpected Blueprint or parent; leaving unchanged: " + package)


def verify_highlights(bridge):
    graph = bridge.call("export_graph", asset=GS, graph="EventGraph")["t3d"]
    nodes = re.findall(r"Begin Object Class=(.*?)\n(.*?)End Object", graph, re.S)
    events = [(kind, body) for kind, body in nodes
              if "K2Node_Event " in kind and 'MemberName="UpdateHighlights"' in body]
    calls = [(kind, body) for kind, body in nodes
             if "K2Node_CallFunction " in kind and 'MemberName="ClearHighlights"' in body]
    if len(events) != 1 or len(calls) != 1 or "K2Node_CallParentFunction" in graph:
        raise RuntimeError("Unexpected highlight graph; inspect it manually before proceeding")
    clear_name = re.search(r'Name="([^"]+)"', calls[0][0]).group(1)
    if "LinkedTo=(" + clear_name + " " not in events[0][1]:
        raise RuntimeError("UpdateHighlights must directly call ClearHighlights")


def compile_blueprint(bridge, package):
    result = bridge.call("compile_blueprint", asset=package)
    if not result.get("ok") or not result.get("saved"):
        raise RuntimeError("Compile/save failed: " + json.dumps(result))
    if result.get("messages"):
        print(json.dumps(result["messages"], indent=2))


def defaults(bridge, package, values):
    result = bridge.call("set_class_defaults", asset=package, defaults=values)
    if result.get("failed") or not result.get("saved"):
        raise RuntimeError("Setting defaults failed: " + json.dumps(result))


def verify_mode_references(bridge):
    dump = bridge.dump(generated(MODE))
    for prop, package in (("GameStateClass", GS), ("InstagibCharacterClass", CHARACTER),
                          ("InstagibRifleClass", RIFLE), ("DefaultPawnClass", CHARACTER)):
        expected = prop + "=BlueprintGeneratedClass'" + generated(package) + "'"
        if expected not in dump:
            raise RuntimeError("Class reference did not resolve: " + prop + " -> " + package)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="Create missing assets and apply the documented xTDM defaults")
    parser.add_argument("--port", type=int, default=int(os.environ.get("MAPFORGE_BRIDGE_PORT", "8765")))
    args = parser.parse_args()
    bridge = Bridge(args.port)
    status = bridge.call("ping")
    if status.get("pie_active") or status.get("building"):
        raise RuntimeError("Stop PIE/build activity before setting up content")
    project = Path(status["project_dir"])
    print("Editor project:", project)

    def exists(package):
        return (project / "Content" / (package.removeprefix("/Game/") + ".uasset")).is_file()

    for package in (CHARACTER, RIFLE):
        if not exists(package):
            raise RuntimeError("Required existing instagib asset is missing: " + package)
    if exists(GS):
        # Export loads the exact package before using OBJ DUMP's name lookup.
        verify_highlights(bridge)
        verify_parent(bridge, GS, GS_PARENT)
        print("Verified stock-parent Blueprint GameState and highlight override")
    native = bridge.dump(MODE_PARENT)
    # OBJ DUMP resolves UClass to its CDO before printing the object name in UE4.15.
    if "NCPlusXTDMGameMode /Script/NetcodePlus.Default__NCPlusXTDMGameMode" not in native:
        raise RuntimeError("New native mode is not loaded. Build/install the editor DLL and reopen the editor first.")
    if exists(MODE):
        bridge.call("export_graph", asset=MODE, graph="EventGraph")
        verify_parent(bridge, MODE, MODE_PARENT)
    if not args.apply:
        if exists(MODE):
            verify_mode_references(bridge)
        print("Native class available; GameState:", exists(GS), "mode Blueprint:", exists(MODE))
        print("Use --apply to create missing assets and apply xTDM defaults.")
        return

    if not exists(GS):
        bridge.call("create_blueprint", package=GS, parent=GS_PARENT, overwrite=False)
        bridge.call("import_graph", asset=GS, graph="EventGraph",
                    t3d=Path(__file__).with_name("GameStateHighlightOverride.t3d").read_text(encoding="utf-8"))
    verify_parent(bridge, GS, GS_PARENT)
    verify_highlights(bridge)
    defaults(bridge, GS, {"SpawnProtectionTime": 0, "RespawnWaitTime": 1})
    compile_blueprint(bridge, GS)
    if not exists(MODE):
        bridge.call("create_blueprint", package=MODE, parent=MODE_PARENT, overwrite=False)
    verify_parent(bridge, MODE, MODE_PARENT)
    defaults(bridge, MODE, {
        "GameStateClass": generated(GS),
        "InstagibCharacterClass": generated(CHARACTER),
        "InstagibRifleClass": generated(RIFLE),
        "DefaultPawnClass": generated(CHARACTER),
        "TeamSize": 2, "DefaultMaxPlayers": 8,
        "TimeLimit": 15, "GoalScore": 0, "MercyScore": 0,
        "RespawnWaitTime": 1, "XTDMSpawnProtectionTime": 0,
        "bAllowIncompleteTeams": False, "bRecordReplays": False,
    })
    compile_blueprint(bridge, MODE)
    verify_mode_references(bridge)
    verify_highlights(bridge)
    print("Saved:", GS, "and", MODE)
    print("Cook NCP_XTDM with the existing instagib dependencies after testing. No build or cook was run.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as exc:
        print("xTDM setup:", exc, file=sys.stderr)
        sys.exit(1)
