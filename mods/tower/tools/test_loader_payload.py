import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("payload", Path(__file__).with_name("build_loader_payload.py"))
payload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(payload)
SCRATCH = Path(__file__).resolve().parents[3] / "build/tower-ui/payload-tests"
SCRATCH.mkdir(parents=True, exist_ok=True)


class FrozenInputTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(dir=SCRATCH))
        self.engine_root = self.temp / "build/release/tower-engine"
        self.version = "v0.3.0-abi4"
        self.folder = self.engine_root / self.version
        self.folder.mkdir(parents=True)
        pe = bytearray(128)
        pe[:2], pe[0x3C:0x40], pe[64:70] = b"MZ", (64).to_bytes(4, "little"), b"PE\0\0\x64\x86"
        self.manifest = {"version": self.version, "module_abi": 4, "commit": "a" * 40, "source_commit": "b" * 40, "files": {}}
        for name in ("PBZCore.dll", "PBZTower.dll"):
            path = self.folder / name
            path.write_bytes(pe)
            self.manifest["files"][name] = {"bytes": len(pe), "sha256": payload.sha(path)}
        self.save()

    def save(self):
        (self.folder / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_valid_matching_pair(self):
        _, manifest, digest = payload.validate_engine(self.engine_root, self.version)
        self.assertEqual(manifest["module_abi"], 4)
        self.assertEqual(len(digest), 64)

    def test_no_development_directory(self):
        with self.assertRaisesRegex(ValueError, "build/core"):
            payload.validate_engine(self.temp / "build/core", self.version)

    def test_tamper_rejected(self):
        with (self.folder / "PBZTower.dll").open("ab") as f:
            f.write(b"new developer build")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            payload.validate_engine(self.engine_root, self.version)

    def test_abi_rejected(self):
        self.manifest["module_abi"] = 3
        self.save()
        with self.assertRaisesRegex(ValueError, "ABI mismatch"):
            payload.validate_engine(self.engine_root, self.version)

    def test_version_traversal_rejected(self):
        with self.assertRaises(ValueError):
            payload.validate_engine(self.engine_root, "../v0.3.0-abi4")

    def test_payload_parity_and_allowlist(self):
        import zipfile
        stage, archive = self.temp / "PBZTower", self.temp / "payload.zip"
        manifest = payload.build(self.engine_root, self.version, archive, stage)
        with zipfile.ZipFile(archive) as z:
            self.assertEqual(set(z.namelist()), {p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file()})
            for name in z.namelist():
                self.assertEqual(z.read(name), (stage / name).read_bytes())
            self.assertFalse(any(x in name.lower() for name in z.namelist() for x in ("dump", "backup", "overlay", "webview", ".sav", "aes-key", "pbzlab.exe")))
        self.assertEqual(len(manifest["files"]), 8)


if __name__ == "__main__":
    unittest.main()
