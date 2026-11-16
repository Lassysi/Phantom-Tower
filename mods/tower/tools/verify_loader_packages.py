"""Offline acceptance of the actual EXE/ZIP. Never opens PBZLab or injects a DLL."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import uuid
import zipfile
from pathlib import Path


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    root = args.evidence.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True)
    cases = []

    def check(condition, name):
        if not condition:
            raise AssertionError(name)
        cases.append(name)

    exe = args.dist.resolve() / "PBZTower-0.2.0-win-x64.exe"
    archive = args.dist.resolve() / "PBZTower-0.2.0-win-x64.zip"
    single = root / "单文件 preview space"
    single.mkdir()
    copy = single / "影塔.exe"
    shutil.copyfile(exe, copy)
    loose = root / "folder preview space"
    loose.mkdir()
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            check(name.startswith("PBZTower/") and not any(p in {".", ".."} for p in name.split("/")) and ":" not in name, "zip path: " + name)
        z.extractall(loose)
    folder = loose / "PBZTower"
    manifest = json.loads((folder / "release-manifest.json").read_text(encoding="utf-8-sig"))
    check(manifest["abi"] == 3 and manifest["engineVersion"] == "v0.2.0-abi3", "approved ABI and engine version")
    check(manifest["coreSha256"] == "d8fde7d4627b4026b7abd15b794f7eeca01d3fa1262c27088939481e187f9a14", "approved frozen core")
    check(manifest["moduleSha256"] == "c5051c44eeac083f4d00568c53ee74aa001ee98c8e15e0a08ac1847ec6ba10f6", "approved frozen module")

    def run(path, user, success=True):
        env = os.environ.copy()
        env.update(DOTNET_MULTILEVEL_LOOKUP="0", DOTNET_ROOT=str(root / "no-machine-runtime"), DOTNET_ROOT_X64=str(root / "no-machine-runtime"),
                   DOTNET_BUNDLE_EXTRACT_BASE_DIR=str(root / "dotnet-cache"))
        result = subprocess.run([str(path), "--self-check", "--mock", "--test-root", str(user)], cwd=path.parent, env=env, capture_output=True, text=True, encoding="utf-8", timeout=50)
        reply = json.loads(result.stdout.strip())
        check((result.returncode == 0 and reply["ok"]) if success else result.returncode != 0 and not reply["ok"], "isolated self-check: " + path.parent.name + (" valid" if success else " damaged"))
        return reply

    user_a, user_b = root / "用户 A", root / "user B"
    a = run(copy, user_a)
    b = run(folder / "PBZTower.exe", user_b)
    check(a["embedded"] and not b["embedded"], "standalone embeds payload; folder edition consumes loose payload")
    check(a["core"] == b["core"] == manifest["coreSha256"] and a["module"] == b["module"] == manifest["moduleSha256"], "both forms declare the same frozen pair")
    cache = next(p for p in (user_a / "packages").iterdir() if re.fullmatch(r"[a-f0-9]{24}", p.name))
    for entry in manifest["files"]:
        check(digest(cache / entry["path"]) == digest(folder / entry["path"]) == entry["sha256"], "payload parity: " + entry["path"])
    declared = {x["path"] for x in manifest["files"]} | {"release-manifest.json", "PBZTower.exe"}
    actual = {p.relative_to(folder).as_posix() for p in folder.rglob("*") if p.is_file()}
    check(actual == declared, "release contains exactly the declared loader-only files")
    check(not any(x in name.lower() for name in actual for x in ("webview", "overlay", "dump", "backup", ".sav", "aes-key", "pbzlab.exe")), "no retired UI, game exports or private files")
    with (cache / "core/PBZCore.dll").open("ab") as f:
        f.write(b"fixture corruption")
    run(copy, user_a)
    check(digest(cache / "core/PBZCore.dll") == manifest["coreSha256"], "standalone repairs damaged cache from immutable embedded payload")
    check(any(".damaged-" in p.name for p in (user_a / "packages").iterdir()), "damaged cache retained for diagnosis")
    with (folder / "modules/PBZTower.dll").open("ab") as f:
        f.write(b"fixture corruption")
    damaged = run(folder / "PBZTower.exe", user_b, False)
    check(damaged["error"] == "ui.package_invalid", "folder tamper rejected before UI or game access")
    result = {"ok": True, "checks": cases, "game_access": False, "source": manifest["source"], "engine": manifest["engineVersion"],
              "exe_sha256": digest(exe), "zip_sha256": digest(archive), "root": str(root)}
    (root / "verification.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"TOWER_PACKAGES_OK {len(cases)} checks; no game connection")
    print(root / "verification.json")


if __name__ == "__main__":
    main()
