"""Build a loader-only payload from one immutable Claude engine release. Never compiles DLLs."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ABI = 4
GAME_BUILD = "26266538"
GAME_SHA256 = "7b4fcd286a350780faf0824d73aa150b21cbf1bccde883e6a96386cbe09973b4"


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def reject_links(path):
    for part in (path, *path.parents):
        if part.exists() and (part.is_symlink() or getattr(part.stat(), "st_file_attributes", 0) & 0x400):
            raise ValueError(f"Reparse/link input rejected: {part}")


def validate_engine(engine_root, version):
    engine_root = Path(engine_root).absolute()
    reject_links(engine_root)
    if [p.casefold() for p in engine_root.parts[-3:]] != ["build", "release", "tower-engine"]:
        raise ValueError("Engine root must end in build/release/tower-engine; build/core is never accepted")
    if not re.fullmatch(r"v\d+\.\d+\.\d+-abi4", version):
        raise ValueError("Frozen engine version must have vX.Y.Z-abi4 format")
    folder = engine_root / version
    reject_links(folder)
    manifest_path = folder / "manifest.json"
    reject_links(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest.get("version") != version or manifest.get("module_abi") != ABI:
        raise ValueError("Frozen manifest version/ABI mismatch")
    if not all(re.fullmatch(r"[0-9a-fA-F]{40}", manifest.get(k, "")) for k in ("commit", "source_commit")):
        raise ValueError("Missing frozen source provenance")
    if set(manifest.get("files", {})) != {"PBZCore.dll", "PBZTower.dll"}:
        raise ValueError("Frozen engine must contain exactly the declared core/module pair")
    for name, info in manifest["files"].items():
        path = folder / name
        reject_links(path)
        if path.stat().st_size != info["bytes"] or sha(path) != info["sha256"].lower():
            raise ValueError(f"Frozen engine checksum mismatch: {name}")
        with path.open("rb") as f:
            if f.read(2) != b"MZ":
                raise ValueError(f"Not a PE image: {name}")
            f.seek(0x3C)
            pe = int.from_bytes(f.read(4), "little")
            f.seek(pe)
            if f.read(6) != b"PE\0\0\x64\x86":
                raise ValueError(f"Not an x64 PE image: {name}")
    return folder, manifest, sha(manifest_path)


def build(engine_root, version, output, folder_output):
    engine, frozen, frozen_hash = validate_engine(engine_root, version)
    output, folder_output = Path(output).absolute(), Path(folder_output).absolute()
    if output.exists() or folder_output.exists():
        raise ValueError("Use fresh payload and staging output paths")
    reject_links(output.parent)
    reject_links(folder_output.parent)
    inputs = {
        "core/PBZCore.dll": (engine / "PBZCore.dll").read_bytes(),
        "modules/PBZTower.dll": (engine / "PBZTower.dll").read_bytes(),
        "engine-manifest.json": (engine / "manifest.json").read_bytes(),
        "README.txt": (ROOT / "mods/tower/ui/README.txt").read_text(encoding="utf-8").replace("@ENGINE_VERSION@", version).encode("utf-8"),
        "CHANGELOG.txt": (ROOT / "mods/tower/ui/CHANGELOG.txt").read_bytes(),
        "notices/MINHOOK-LICENSE.txt": (ROOT / "src/core/third_party/minhook/LICENSE.txt").read_bytes(),
        "notices/DOTNET-LICENSE.txt": (ROOT / "tools/release/notices/LICENSE.txt").read_bytes(),
        "notices/DOTNET-THIRD-PARTY-NOTICES.txt": (ROOT / "tools/release/notices/THIRD-PARTY-NOTICES.txt").read_bytes(),
    }
    # Input set is deliberately literal. No workspace/build directory walk is permitted.
    entries = [{"path": name, "length": len(data), "sha256": hashlib.sha256(data).hexdigest()} for name, data in sorted(inputs.items())]
    manifest = {"schema": 1, "version": "0.3.0", "source": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "abi": ABI, "protocol": 2, "gameBuild": GAME_BUILD, "gameExeSha256": GAME_SHA256,
                "coreSha256": frozen["files"]["PBZCore.dll"]["sha256"], "moduleSha256": frozen["files"]["PBZTower.dll"]["sha256"],
                "engineVersion": version, "engineManifestSha256": frozen_hash, "files": entries}
    inputs["release-manifest.json"] = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")
    folder_output.mkdir(parents=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data in sorted(inputs.items()):
            archive.writestr(name, data)
            path = folder_output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    # Recheck immutable inputs after copying; do not package a concurrently changed pair.
    _, second, second_hash = validate_engine(engine_root, version)
    if frozen != second or frozen_hash != second_hash:
        raise ValueError("Frozen release changed while packaging")
    for entry in entries:
        print(f"{entry['sha256']}  {entry['path']}")
    print("PAYLOAD_OK " + sha(output))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--engine-version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--folder", type=Path, required=True)
    args = parser.parse_args()
    build(args.engine_root, args.engine_version, args.output, args.folder)


if __name__ == "__main__":
    main()
