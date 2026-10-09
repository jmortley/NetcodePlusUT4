"""Compile the actual 329 anchor helper and policy against a small UE adapter.

Exercises admission, physics-query guards, single-use markers, identity/epoch
invalidation, cleanup, and queue age/cap accounting. This is not a UBT, physics,
replication or live network test.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_wipeout_healing import find_compiler

PLUGIN = Path(os.environ.get("NCP_TEST_SOURCE", Path(__file__).resolve().parents[2]))

class FireAnchor329Tests(unittest.TestCase):
    def test_actual_helper_policy_and_lifecycle(self):
        compiler, env, msvc = find_compiler()
        with tempfile.TemporaryDirectory(prefix="ncp-fire-anchor329-") as temporary:
            directory = Path(temporary)
            for source, name in (("Source/Private/NCFireAnchor.cpp", "NCFireAnchor.cpp"),
                                 ("Source/Public/NCFireAnchor.h", "NCFireAnchor.h"),
                                 ("Source/Public/NCFireAnchorPolicy.h", "NCFireAnchorPolicy.h"),
                                 ("tools/tests/fire_anchor329_adapter.h", "anchor_adapter.h"),
                                 ("tools/tests/fire_anchor329_cases.cpp", "cases.cpp")):
                (directory/name).write_bytes((PLUGIN/source).read_bytes())
            for name in ("NetcodePlus.h", "UnrealTournament.h", "UTCharacter.h", "UTCharacterMovement.h",
                         "UTWeapon.h", "GameFramework/PlayerController.h", "Engine/World.h",
                         "Engine/NetConnection.h", "Engine/NetDriver.h"):
                path=directory/name
                path.parent.mkdir(parents=True,exist_ok=True)
                path.write_text('#include "anchor_adapter.h"\n',encoding="utf-8")
            executable=directory/("anchor.exe" if os.name=="nt" else "anchor")
            if msvc:
                command=[compiler,"/nologo","/EHsc","/W4","/WX","/wd4100","/std:c++14",f"/I{directory}",str(directory/"cases.cpp"),f"/Fe{executable}"]
            else:
                command=[compiler,"-std=c++14","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-pedantic",f"-I{directory}",str(directory/"cases.cpp"),"-o",str(executable)]
            built=subprocess.run(command,cwd=directory,env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            run=subprocess.run([str(executable)],cwd=directory,env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn("anchor lifecycle and policy checks",run.stdout)
            print(run.stdout.strip())

if __name__=="__main__":
    unittest.main()
