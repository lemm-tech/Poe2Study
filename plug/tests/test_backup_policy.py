"""Exercise backup retention in disposable fixture projects; never touches game processes."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "plug/.build/backup_policy"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    fixture = Path(tempfile.mkdtemp(prefix="fixture-", dir=OUT))
    (fixture / "plug").mkdir()
    (fixture / "plug/Poe2Study.sln").write_text("fixture\n", encoding="utf-8")
    (fixture / "plug/main.cpp").write_text("int value = 0;\n", encoding="utf-8")
    (fixture / "plug/config.ini").write_text("[game]\n", encoding="utf-8")
    (fixture / "docs").mkdir()
    (fixture / "docs/说明.md").write_text("fixture document\n", encoding="utf-8")
    (fixture / ".gitignore").write_text("/bin/\n", encoding="utf-8")
    (fixture / "plug/assets").mkdir()
    (fixture / "plug/assets/icon.png").write_bytes(b"fixture-image")
    for ignored in ["plug/bin/config.ini", "plug/.build/generated.cpp", "plug/profiles/session/config.ini",
                    "版本管理/nested.cpp", "plug/test.exe", "plug/test.dll", "plug/trace.log"]:
        path = fixture / ignored
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"ignored")

    shell = shutil.which("powershell.exe")
    if not shell:
        raise RuntimeError("Windows PowerShell not found")
    log = []

    def run(*args, fail=False, shell_override=None):
        result = subprocess.run([shell_override or shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                 str(ROOT / "备份项目.ps1"), "-ProjectRoot", str(fixture), *args],
                                capture_output=True, timeout=30)
        text = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        log.append({"args": args, "returncode": result.returncode, "output": text})
        if fail:
            assert result.returncode != 0, text
        else:
            assert result.returncode == 0, text
        return text

    def snapshots():
        return sorted(path for path in (fixture / "版本管理").iterdir()
                      if path.is_dir() and len(path.name) == 14 and path.name.isdecimal())

    run("-Task", "fixture baseline")
    assert len(snapshots()) == 1
    first = snapshots()[0]
    manifest = json.loads((first / "源码校验.json").read_text(encoding="utf-8-sig"))
    assert {entry["Path"] for entry in manifest} == {
        ".gitignore", "docs/说明.md", "plug/Poe2Study.sln", "plug/main.cpp",
        "plug/config.ini", "plug/assets/icon.png"}
    run()
    assert snapshots() == [first], "unchanged files created another backup"
    if shutil.which("pwsh.exe"):
        run(shell_override=shutil.which("pwsh.exe"))
        assert snapshots() == [first], "shell version changed the content fingerprint"
    (fixture / "plug/.build/generated.cpp").write_text("different artifact", encoding="utf-8")
    run()
    assert snapshots() == [first], "generated artifacts affected deduplication"
    run("-Milestone", "fixture protected")
    assert snapshots() == [first] and (first / "里程碑.json").is_file()

    for value in range(1, 8):
        (fixture / "plug/main.cpp").write_text(f"int value = {value};\n", encoding="utf-8")
        run()
    kept = snapshots()
    assert len(kept) == 6 and first in kept, "expected latest five plus older milestone"
    for snapshot in kept:
        assert (snapshot / "源码校验.json").is_file()
    assert (kept[-1] / "plug/main.cpp").read_text(encoding="utf-8") == "int value = 7;\n"
    run("-MarkMilestone", first.name, "-Milestone", "fixture renamed")
    assert snapshots() == kept
    assert json.loads((first / "里程碑.json").read_text(encoding="utf-8-sig"))["name"] == "fixture renamed"
    run("-MarkMilestone", "../outside", "-Milestone", "bad path", fail=True)
    assert snapshots() == kept

    # Deletions and additions must change the fingerprint, not only changed file bytes.
    (fixture / "docs/说明.md").unlink()
    (fixture / "docs/new.md").write_text("new document", encoding="utf-8")
    run()
    latest = snapshots()[-1]
    assert (latest / "docs/new.md").is_file() and not (latest / "docs/说明.md").exists()
    assert len(snapshots()) == 6
    (OUT / "results.json").write_text(json.dumps({"fixture": str(fixture), "checks": [
        "source/config/docs/assets included", "artifacts and recursive archives excluded",
        "unchanged source deduplicated across PowerShell 5/7", "generated changes ignored", "source edits detected",
        "latest five retained", "older milestone protected", "milestone tagging deduplicated",
        "traversal rejected", "file additions/deletions detected"], "runs": log},
        ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: backup selection, deduplication, latest-five retention, milestones, path guards")
    print("Fixture:", fixture)
    print("Results:", OUT / "results.json")


if __name__ == "__main__":
    main()
