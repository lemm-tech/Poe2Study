"""Build/run isolated navigation tests. Never injects a DLL or connects to a game."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".build" / "tests" / "navigation"
SHARED = ["Monitor/NamedPipeServer.cpp", "Plug/NamedPipeClie.cpp", "Plug/Navigator.cpp",
          "Plug/MapSink.cpp", "Plug/BmpWriter.cpp", "Common/AppConfig.cpp", "Common/Logger.cpp"]
SUITES = {"core": ["tests/tests.cpp"],
          "portal_return": ["tests/portal_return_tests.cpp"],
          "encounter": ["tests/encounter_tests.cpp"],
          "ritual_state": ["tests/ritual_state_tests.cpp"],
          "ritual_combat": ["tests/ritual_combat_tests.cpp"],
          "recovery": ["tests/recovery_tests.cpp"],
          "view_controls": ["tests/view_controls_tests.cpp"],
          "launcher": ["tests/game_launcher_tests.cpp", "Plug/GameLauncher.cpp"],
          "combo": ["tests/combo_tests.cpp", "Plug/ComboConfig.cpp", "Monitor/IdSkillCaster.cpp",
                    "Common/AppConfig.cpp", "Common/Logger.cpp"],
          "feedback": ["tests/nav_feedback_tests.cpp"] + SHARED,
          "managed": ["tests/managed_tests.cpp"] + SHARED,
          "follow": ["tests/follow_tests.cpp", "Plug/FollowRunner.cpp", "Plug/Surroundings.cpp",
                     "Plug/MyOLAFunction.cpp", "Plug/WorldArea.cpp", "Plug/PartyData.cpp",
                     "Plug/UseCases.cpp", "Plug/Inventory.cpp", "Plug/SkillSlots.cpp", "Common/OlaDriver.cpp", "Common/GameMemory.cpp",
                     "Common/Entity.cpp", "Common/Component.cpp", "Common/PointerChain.cpp"] + SHARED}
SUITES["ritual_roll"] = ["tests/ritual_roll_tests.cpp", "Plug/ComboRunner.cpp"] + SUITES["follow"][1:]
SUITES["entity_scan"] = ["tests/entity_scan_tests.cpp", "Plug/Surroundings.cpp", "Plug/WorldArea.cpp",
                         "Common/Entity.cpp", "Common/Component.cpp", "Common/GameMemory.cpp",
                         "Common/OlaDriver.cpp", "Common/PointerChain.cpp", "Common/AppConfig.cpp", "Common/Logger.cpp"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=list(SUITES))
    args = parser.parse_args()
    env = dict(os.environ)
    # A stray quote in the host PATH breaks vcvarsall's parenthesized cmd blocks.
    # Normalize only this child environment, never the user's persisted PATH.
    env["PATH"] = ";".join(part.strip('"') for part in env.get("PATH", "").split(";"))
    locator = Path(env.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    found = subprocess.run([str(locator), "-latest", "-products", "*", "-requires",
                            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                           capture_output=True, text=True, env=env)
    if found.returncode or not found.stdout.strip():
        print("Visual Studio C++ tools not found.")
        return 2
    vcvars = Path(found.stdout.strip()) / "VC/Auxiliary/Build/vcvars64.bat"
    for name in ([args.suite] if args.suite else SUITES):
        folder = OUT / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "config.ini").write_bytes((ROOT / "Plug/config.ini").read_bytes())
        batch = folder / "build.cmd"
        exe = folder / (name + ".exe")
        sources = " ".join('"' + str(ROOT / file) + '"' for file in SUITES[name])
        batch.write_text('@echo off\nchcp 65001 >nul\ncall "' + str(vcvars) +
                         '" >nul 2>&1\nif errorlevel 1 exit /b %errorlevel%\n' +
                         'cl /nologo /EHsc /MT /O2 /utf-8 /std:c++17 /DNOMINMAX ' + sources +
                         ' /Fo:"' + folder.as_posix() + '/" /Fe:"' + str(exe) + '" /link user32.lib\n', encoding="utf-8")
        print("BUILD", name, flush=True)
        with (folder / "build.log").open("wb") as log:
            rc = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=ROOT, env=env,
                                stdout=log, stderr=subprocess.STDOUT).returncode
        if rc:
            print((folder / "build.log").read_text(encoding="utf-8", errors="replace"))
            return rc
        print("RUN", name, flush=True)
        try:
            with (folder / "run.log").open("wb") as log:
                rc = subprocess.run([str(exe)], cwd=folder, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=120).returncode
        except subprocess.TimeoutExpired:
            print("TIMEOUT", folder / "run.log")
            return 124
        output = (folder / "run.log").read_text(encoding="utf-8", errors="replace")
        print(output if rc else "\n".join(output.splitlines()[-7:]))
        print("LOG", folder / "run.log")
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
