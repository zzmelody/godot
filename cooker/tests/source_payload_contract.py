"""Cooker immutable-source API contract; no render or asset acceptance claims."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--cooker", type=Path, required=True)
options, remaining = parser.parse_known_args()
COOKER = options.cooker.resolve()


class SourcePayloadContract(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.root = Path(self.workspace.name)
        (self.root / "project.godot").write_text("config_version=5\n")
        self.source = self.root / "content/shared/source/ui/font.otf"
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b"OTTO\x00immutable font bytes")
        self.output = self.root / "content/shared/generated/material/ui-fonts/font.otf"

    def tearDown(self):
        self.workspace.cleanup()

    def pipeline(self, code, ok=True):
        (self.root / "pipeline.luau").write_text(code, encoding="utf-8")
        environment = dict(os.environ, PATH=str(self.root / "no-executables"))
        result = subprocess.run([str(COOKER), "--path", str(self.root), "--pipeline", "res://pipeline.luau"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", env=environment)
        report = json.loads(next(line for line in result.stdout.splitlines() if line.startswith("{")))
        self.assertEqual(report["ok"], ok, report)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return report

    def declaration(self, source="res://content/shared/source/ui/font.otf",
                    output="res://content/shared/generated/material/ui-fonts/font.otf"):
        return f'''return {{{{operation="stage-source",source="{source}",output="{output}",if_missing=true}},
        {{operation="asset-manifest",output="res://content/shared/generated/material/ui-fonts/asset.manifest.json",
        asset_id="ui-fonts",revision=1,kind="material",entry="font.otf",files={{"font.otf"}},if_missing=true}}}}'''

    def test_bytes_manifest_cache_and_source_change(self):
        code = self.declaration()
        self.pipeline(code)
        self.assertEqual(self.output.read_bytes(), self.source.read_bytes())
        manifest = json.loads((self.output.parent / "asset.manifest.json").read_text())
        self.assertEqual(manifest["files"][0]["sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertTrue(all(step["skipped"] for step in self.pipeline(code)["steps"]))
        self.source.write_bytes(b"OTTO\x00changed source")
        self.pipeline(code)
        self.assertEqual(self.output.read_bytes(), self.source.read_bytes())

    def test_output_tampering_is_rejected(self):
        code = self.declaration()
        self.pipeline(code)
        self.output.write_bytes(b"tampered")
        self.pipeline(code, ok=False)

    def test_scope_extension_and_missing_source_rejection(self):
        cases = [
            ("res://content/shared/source/ui/font.otf", "res://content/worlds/other/generated/material/ui/font.otf"),
            ("res://content/shared/source/ui/font.otf", "res://content/shared/generated/material/ui-fonts/font.png"),
            ("res://content/shared/source/ui/font.otf", "res://content/shared/source/ui/copy.otf"),
            ("res://content/shared/source/ui/missing.otf", "res://content/shared/generated/material/ui-fonts/font.otf"),
            ("res://content/shared/source/ui/font.otf", "res://content/shared/generated/material/ui-fonts/../font.otf"),
        ]
        for source, output in cases:
            with self.subTest(source=source, output=output):
                self.pipeline(self.declaration(source, output), ok=False)
        self.source.with_suffix(".luau").write_text("return {}")
        self.pipeline(self.declaration("res://content/shared/source/ui/font.luau",
                                      "res://content/shared/generated/material/ui-fonts/font.luau"), ok=False)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining])
