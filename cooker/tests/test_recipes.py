# /*<<----- VEYA_COOKER: native recipe integration and fault tests; Python is only the test harness. */
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--cooker", type=Path, required=True)
parser.add_argument("--reference-godot", type=Path)
arguments, remaining = parser.parse_known_args()
EXECUTABLE = arguments.cooker.resolve()
REFERENCE = arguments.reference_godot.resolve() if arguments.reference_godot else None
ROOT = Path(__file__).resolve().parents[2]
BOX = 'return cooker.primitive("box", {size={2,3,4}})'


class RecipeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="veya-recipe-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        (self.project / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Recipe Test"\n')
        self.index = 0

    def job(self, job, success=True):
        self.index += 1
        path = self.project / f"job-{self.index}.json"
        path.write_text(json.dumps(job))
        environment = os.environ.copy()
        environment["PATH"] = str(self.project / "no-external-executables")
        run = subprocess.run([str(EXECUTABLE), "--path", str(self.project), "--job", str(path)],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20, env=environment)
        self.assertGreaterEqual(run.returncode, 0, run.stdout + run.stderr)  # No signal/crash on bad input.
        lines = [line for line in run.stdout.splitlines() if line.startswith('{')]
        self.assertTrue(lines, run.stdout + run.stderr)
        result = json.loads(lines[-1])
        self.assertEqual(result["ok"], success, str(result) + run.stderr)
        self.assertEqual(run.returncode == 0, success, run.stdout + run.stderr)
        if success:
            self.assertNotIn("ERROR:", run.stdout + run.stderr)
        self.assertNotIn("leaked", run.stderr.lower(), run.stderr)
        return result

    def recipe(self, code=BOX, success=True, **fields):
        source = self.project / f"recipe-{self.index}.luau"
        source.write_bytes(code if isinstance(code, bytes) else code.encode())
        output = fields.pop("output", f"res://content/worlds/fixture/generated/result-{self.index}.res")
        result = self.job({"operation": "run-recipe", "source": "res://" + source.name,
                           "output": output, **fields}, success)
        target = self.project / output.removeprefix("res://")
        if success:
            self.assertTrue(target.is_file())
            self.assertEqual(result["source_sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(result["sha256"], hashlib.sha256(target.read_bytes()).hexdigest())
        elif output.startswith("res://content/worlds/fixture/generated/"):
            self.assertFalse(target.exists(), result)
        self.assertFalse(list(self.project.rglob("*.recipe-*")))
        return result

    def pipeline(self, code, success=True):
        source = self.project / f"pipeline-{self.index}.luau"
        source.write_bytes(code if isinstance(code, bytes) else code.encode())
        environment = os.environ.copy()
        environment["PATH"] = str(self.project / "no-external-executables")
        run = subprocess.run([str(EXECUTABLE), "--path", str(self.project), "--pipeline", "res://" + source.name],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20, env=environment)
        lines = [line for line in run.stdout.splitlines() if line.startswith('{')]
        self.assertTrue(lines, run.stdout + run.stderr)
        result = json.loads(lines[-1])
        self.assertEqual(result["ok"], success, result)
        self.assertEqual(run.returncode == 0, success, run.stdout + run.stderr)
        if success:
            self.assertNotIn("ERROR:", run.stdout + run.stderr)
        return result

    def test_capabilities_and_no_external_runtime(self):
        run = subprocess.run([str(EXECUTABLE), "--capabilities"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
        report = json.loads(next(line for line in run.stdout.splitlines() if line.startswith('{')))
        self.assertEqual(report["script_languages"], 0)
        self.assertEqual(report["recipe_runtime"]["version"], "0.738")
        self.assertEqual(report["recipe_runtime"]["api_version"], 11)
        for name in ("field", "sample", "field_info", "points", "child_names", "children", "parts"):
            self.assertIn(name, report["recipe_runtime"]["apis"])
        self.assertIn("scene", report["recipe_runtime"]["apis"])
        self.assertIn("virtual_geometry", report["recipe_runtime"]["apis"])
        self.assertIn("procedural_texture", report["recipe_runtime"]["apis"])
        self.assertIn("effect", report["recipe_runtime"]["apis"])
        self.assertEqual(report["pipeline_runtime"]["language"], "Luau")
        self.assertEqual(report["pipeline_runtime"]["api_version"], 1)
        # All recipe jobs below run with PATH pointing at an empty/nonexistent directory.
        report = self.recipe(output="res://assets/runtime/model/basic/box.res")
        self.assertEqual(report["type"], "BoxMesh")
        self.assertGreater(report["vertices"], 0)

    def test_luau_pipeline_owns_generation_validation_manifest_and_pack(self):
        (self.project / "asset.luau").write_text(BOX)
        code = '''local output = "res://content/worlds/fixture/generated/example/example.res"
return {
 {name="generate",operation="run-recipe",source="res://asset.luau",output=output,if_missing=true},
 {name="validate",operation="validate-resource",source=output,type="BoxMesh"},
 {name="manifest",operation="asset-manifest",output="res://content/worlds/fixture/generated/example/asset.manifest.json",
  asset_id="example",revision=1,kind="model",entry="example.res",files={"example.res"},
  source="test-source",generator="test-generator",if_missing=true},
 {name="pack",operation="pack",files={output},output="res://content/releases/example.pck",if_missing=true},
}'''
        result = self.pipeline(code)
        self.assertEqual(result["step_count"], 4)
        self.assertTrue(all(step["ok"] for step in result["steps"]))
        manifest = json.loads((self.project / "content/worlds/fixture/generated/example/asset.manifest.json").read_text())
        self.assertEqual(manifest["pipeline"], "veya-asset-cooker-luau")
        self.assertEqual(manifest["entry"], "example.res")
        self.assertEqual(len(manifest["files"][0]["sha256"]), 64)
        repeated = self.pipeline(code)
        self.assertTrue(repeated["steps"][0]["skipped"])
        self.assertFalse(repeated["steps"][1].get("skipped", False))
        self.assertTrue(repeated["steps"][2]["skipped"])
        self.assertTrue(repeated["steps"][3]["skipped"])

    def test_module_package_pipeline_paths(self):
        for world_root in ("worlds", "mod-worlds"):
            for collection in ("modules", "props", "npcs"):
                with self.subTest(world_root=world_root, collection=collection):
                    package = f"content/{world_root}/fixture/{collection}/fixture_asset"
                    folder = self.project / package
                    (folder / "cooker").mkdir(parents=True)
                    (folder / "source").mkdir()
                    (folder / "cooker/recipe.luau").write_text(BOX)
                    payload = b"uid://immutable-module-payload\n"
                    (folder / "source/model.uid").write_bytes(payload)
                    output = f"res://{package}/generated/model/fixture/model.res"
                    uid = output + ".uid"
                    steps = f'''return {{
 {{operation="run-recipe",source="res://{package}/cooker/recipe.luau",output="{output}"}},
 {{operation="stage-source",source="res://{package}/source/model.uid",output="{uid}"}},
 {{operation="asset-manifest",output="res://{package}/generated/model/fixture/asset.manifest.json",
  asset_id="fixture",revision=1,kind="model",entry="model.res",files={{"model.res","model.res.uid"}}}},
}}'''
                    (folder / "cooker/steps.luau").write_text(steps)
                    report = self.pipeline(f'return pipeline.import("res://{package}/cooker/steps.luau","steps")')
                    self.assertEqual(report["step_count"], 3)
                    self.assertTrue(all(step["ok"] for step in report["steps"]))
                    self.assertEqual((folder / "generated/model/fixture/model.res.uid").read_bytes(), payload)
                    generated = folder / "generated/model/fixture/model.res"
                    manifest = json.loads((generated.parent / "asset.manifest.json").read_text())
                    recorded = {entry["path"]: entry["sha256"] for entry in manifest["files"]}
                    self.assertEqual(recorded["model.res"], hashlib.sha256(generated.read_bytes()).hexdigest())
                    self.assertEqual(recorded["model.res.uid"], hashlib.sha256(payload).hexdigest())

    def test_nested_spatial_inventory_exceeds_the_old_conversion_allowance(self):
        folder = self.project / "content/worlds/fixture/modules/cells/cooker"
        folder.mkdir(parents=True)
        (folder / "inventory.luau").write_text('''local cells = {}
for i = 1, 12000 do cells[i] = {x=i, z=-i, kind="forest"} end
return {cells=cells}
''')
        (folder / "jobs.luau").write_text('''return pipeline.import(
"res://content/worlds/fixture/modules/cells/cooker/inventory.luau", "data")
''')
        (self.project / "asset.luau").write_text(BOX)
        result = self.pipeline('''local inventory = pipeline.import(
"res://content/worlds/fixture/modules/cells/cooker/jobs.luau", "data")
assert(#inventory.cells == 12000 and inventory.cells[12000].z == -12000)
return {{operation="run-recipe", source="res://asset.luau",
output="res://assets/runtime/model/cells/marker.res"}}
''')
        self.assertEqual(result["step_count"], 1)
        self.assertTrue(result["steps"][0]["ok"])

    def test_shared_recipe_inputs_keep_one_validated_resource_per_batch(self):
        source = "res://assets/runtime/model/shared-input/model.scn"
        self.recipe('''local mesh = cooker.primitive("box", {size={2,3,4}})
return cooker.scene({
 {asset=mesh, color={0.8,0.2,0.1,1}, custom_data={1,0,0,0}},
 {asset=mesh, position={3,0,0}, color={0.1,0.8,0.2,1}, custom_data={0,1,0,0}},
})''', output=source)
        (self.project / "view.luau").write_text('''local mesh = cooker.input("model")
return cooker.scene({{asset=mesh,position={cooker.parameters.x,0,0}}})
''')
        result = self.pipeline('''local steps = {}
for x = 1, 3 do
  table.insert(steps, {operation="run-recipe",source="res://view.luau",
    output="res://assets/runtime/world/shared-views/cell_"..x..".scn",
    inputs={model="res://assets/runtime/model/shared-input/model.scn"},
    parameters={x=x}})
end
return steps
''')
        if os.name == "nt":
            # Both consumers reuse the input. Receipt fingerprinting may
            # consult the same frozen proof before recipe execution as well.
            self.assertGreaterEqual(result["shared_input_reuses"], 2)
        else:
            self.assertEqual(result["shared_input_reuses"], 0)
        self.assertTrue(all(step["ok"] for step in result["steps"]))

    @unittest.skipUnless(os.name == "nt", "Windows shared-input file leases")
    def test_shared_input_lease_blocks_writes_and_ends_with_the_pipeline(self):
        source = "res://assets/runtime/model/shared-lease/model.res"
        self.recipe(output=source)
        asset = self.project / source.removeprefix("res://")
        original = asset.read_bytes()
        (self.project / "lease.luau").write_text('''local mesh = cooker.input("model")
if cooker.parameters.wait then while true do end end
return cooker.scene({{asset=mesh}})
''')
        (self.project / "lease.pipeline.luau").write_text('''return {
 {operation="run-recipe",source="res://lease.luau",parameters={wait=false},
  inputs={model="res://assets/runtime/model/shared-lease/model.res"},
  output="res://assets/runtime/world/shared-lease/ready.scn"},
 {operation="run-recipe",source="res://lease.luau",parameters={wait=true},
  inputs={model="res://assets/runtime/model/shared-lease/model.res"},
  limits={time_ms=500},output="res://assets/runtime/world/shared-lease/wait.scn"},
}
''')
        process = subprocess.Popen(
            [str(EXECUTABLE), "--path", str(self.project), "--pipeline", "res://lease.pipeline.luau"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        ready = self.project / "assets/runtime/world/shared-lease/ready.scn"
        deadline = time.monotonic() + 5
        while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.002)
        self.assertTrue(ready.is_file(), "first output must publish before checking the input lease")
        with self.assertRaises(PermissionError):
            asset.write_bytes(original)
        output, errors = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0, output + errors)
        result = json.loads(next(line for line in output.splitlines() if line.startswith('{')))
        self.assertIn("budget exceeded", result["steps"][-1]["message"])
        self.assertEqual(asset.read_bytes(), original)
        asset.write_bytes(original)

    def test_shared_input_cache_does_not_freeze_a_declared_producer_directory(self):
        source = "res://assets/runtime/model/shared-update/model.res"
        replacement = "res://assets/runtime/model/replacement/model.res"
        self.recipe(output=source)
        self.recipe('return cooker.primitive("box", {size={4,3,4}})', output=replacement)
        (self.project / "inspect.luau").write_text('''local mesh = cooker.input("model")
local bounds = cooker.bounds(mesh)
assert(bounds.max[1] - bounds.min[1] == cooker.parameters.width)
return cooker.scene({{asset=mesh}})
''')
        result = self.pipeline('''return {
 {operation="run-recipe",source="res://inspect.luau",parameters={width=2},
  inputs={model="res://assets/runtime/model/shared-update/model.res"},
  output="res://assets/runtime/world/shared-update/before.scn"},
 {operation="asset-manifest",asset_id="before-update",revision=1,kind="world",entry="before.scn",
  output="res://assets/runtime/world/shared-update/asset.manifest.json",files={"before.scn"}},
 {operation="save-resource",source="res://assets/runtime/model/replacement/model.res",
  output="res://assets/runtime/model/shared-update/model.res"},
 {operation="run-recipe",source="res://inspect.luau",parameters={width=4},
  inputs={model="res://assets/runtime/model/shared-update/model.res"},
  output="res://assets/runtime/world/after-update/after.scn"},
 {operation="asset-manifest",asset_id="after-update",revision=1,kind="world",entry="after.scn",
  output="res://assets/runtime/world/after-update/asset.manifest.json",files={"after.scn"}},
}
''')
        self.assertEqual(result["shared_input_reuses"], 0)
        self.assertTrue(all(step["ok"] for step in result["steps"]))
        for folder, width in (("shared-update", 2), ("after-update", 4)):
            published = json.loads((self.project / "assets/runtime/world" / folder / "asset.manifest.json").read_text())
            record = published["files"][0]["streaming"]
            self.assertEqual(record["maximum"][0] - record["minimum"][0], width)

    def test_module_package_paths_reject_wrong_owner_and_intermediate_folders(self):
        package = "content/worlds/fixture/props/fixture_asset"
        folder = self.project / package
        (folder / "source").mkdir(parents=True)
        (folder / "source/model.uid").write_bytes(b"uid://immutable-module-payload\n")
        outputs = (
            "content/worlds/fixture/middle/props/fixture_asset/generated/model.res",
            "content/worlds/fixture/props/fixture_asset/middle/generated/model.res",
            "content/worlds/fixture/props/generated/model.res",
            "content/worlds/fixture/other/fixture_asset/generated/model.res",
            "content/worlds/fixture/props//generated/model.res",
            "content/worlds/fixture/props/fixture_asset/generated/../escape.res",
        )
        for output in outputs:
            with self.subTest(output=output):
                self.recipe(output="res://" + output, success=False)
                self.assertFalse((self.project / output).exists())
        for owner in ("content/worlds/other/props/fixture_asset",
                      "content/worlds/fixture/props/another_asset",
                      "content/worlds/fixture",
                      "content/shared"):
            with self.subTest(owner=owner):
                output = f"{owner}/generated/model/refused/model.uid"
                self.pipeline(f'return {{{{operation="stage-source",source="res://{package}/source/model.uid",output="res://{output}"}}}}', success=False)
                self.assertFalse((self.project / output).exists())
        for directory in ("content/worlds/fixture/props/fixture_asset/middle/cooker",
                          "content/worlds/fixture/middle/props/fixture_asset/cooker"):
            with self.subTest(directory=directory):
                steps = self.project / directory / "steps.luau"
                steps.parent.mkdir(parents=True)
                steps.write_text(f'return {{{{operation="stage-source",source="res://{package}/source/model.uid",output="res://{package}/generated/model/refused/model.uid"}}}}')
                self.pipeline(f'return pipeline.import("res://{directory}/steps.luau","steps")', success=False)
        self.assertFalse((folder / "generated").exists())

    def test_luau_pipeline_rejects_non_declarative_or_unsafe_values(self):
        cached = self.project / "content/worlds/fixture/generated/cached.res"
        cached.parent.mkdir(parents=True)
        cached.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\n')
        for code in ("return {}", "return {{operation='validate-resource', source=os.getenv('X')}}",
                     "return {{operation='validate-resource', source=function() end}}",
                     "return {{[1]='mixed',operation='validate-resource'}}",
                     "return {{operation='unknown',output='res://content/worlds/fixture/generated/cached.res',if_missing=true}}",
                     "return {{operation='validate-resource',source='res://content/worlds/fixture/generated/cached.res',if_missing=true}}",
                     "while true do end"):
            with self.subTest(code=code[:40]):
                self.pipeline(code, success=False)

    def test_virtual_geometry_material_displacement_publication(self):
        raw = self.project / "assets/raw/material/bounded"
        raw.mkdir(parents=True)
        source = raw / "source.tres"
        source.write_text('[gd_resource type="ShaderMaterial" load_steps=2 format=3]\n'
                          '[sub_resource type="Shader" id="Shader"]\n'
                          'code = "shader_type spatial; void vertex(){VERTEX.x+=0.1;}"\n'
                          '[resource]\nshader = SubResource("Shader")\n', encoding="utf-8")
        original = source.read_bytes()
        output = "assets/runtime/material/bounded/material.res"
        code = ('return {{operation="save-resource",source="res://assets/raw/material/bounded/source.tres",'
                'output="res://' + output + '",type="ShaderMaterial",max_vertex_displacement=0.794}}')
        result = self.pipeline(code)["steps"][0]
        self.assertAlmostEqual(result["max_vertex_displacement"], 0.794, places=6)
        saved = (self.project / output).read_bytes()
        loaded = self.job({"operation": "validate-resource", "source": "res://" + output,
                           "type": "ShaderMaterial"})
        self.assertAlmostEqual(loaded["max_vertex_displacement"], 0.794, places=6)
        self.assertTrue(self.pipeline(code)["steps"][0]["skipped"])
        changed = self.pipeline(code.replace("=0.794", "=0.9"))["steps"][0]
        self.assertAlmostEqual(changed["max_vertex_displacement"], 0.9, places=6)
        self.assertNotEqual((self.project / output).read_bytes(), saved)
        updated = (self.project / output).read_bytes()
        self.pipeline(code.replace("=0.794", "=-1"), success=False)
        self.assertEqual((self.project / output).read_bytes(), updated)
        self.assertEqual(source.read_bytes(), original)
        for index, value in enumerate(("-1", "math.huge", "0/0", '"0.5"', "true")):
            rejected = f"assets/runtime/material/bounded/rejected-{index}.res"
            self.pipeline(code.replace(output, rejected).replace("=0.794", "=" + value), success=False)
            self.assertFalse((self.project / rejected).exists())
        box = raw / "box.tres"
        box.write_text('[gd_resource type="BoxMesh" format=3]\n[resource]\n')
        self.pipeline(code.replace("source.tres", "box.tres").replace("ShaderMaterial", "BoxMesh")
                      .replace(output, "assets/runtime/material/bounded/box.res"), success=False)
        self.assertFalse((self.project / "assets/runtime/material/bounded/box.res").exists())
        zero = self.pipeline(code.replace(output, "assets/runtime/material/bounded/zero.res")
                             .replace("=0.794", "=0"))["steps"][0]
        self.assertEqual(zero["max_vertex_displacement"], 0)

    def test_virtual_geometry_builds_root_and_content_addressed_container(self):
        result = self.recipe('''
local source = cooker.primitive("box", {size={2,3,4}})
local first = cooker.virtual_geometry(source, {})
local second = cooker.virtual_geometry(source, {})
local bounds = cooker.bounds(second)
assert(bounds.max[1] >= 1 and bounds.min[1] <= -1)
return second
''', output="res://assets/runtime/model/vg/model.res")
        self.assertEqual(result["type"], "VirtualGeometryMesh")
        self.assertEqual(result["virtual_geometry_builds"], 1)
        report = result["virtual_geometry"]
        self.assertEqual(report["source_triangles"], 12)
        self.assertEqual(report["source_surfaces"], 1)
        self.assertEqual(result["vertices"], 24)
        self.assertEqual(report["verified_pages"], report["pages"])
        self.assertLessEqual(report["gpu_payload_bytes"], report["gpu_page_capacity_bytes"])
        self.assertFalse(report["render_ready"])
        self.assertEqual(len(report["build_key"]), 64)
        files = result["geometry_outputs"]
        self.assertEqual(len(files), 1)
        container = self.project / files[0]["path"].removeprefix("res://")
        data = container.read_bytes()
        self.assertEqual(data[:4], b"VGPG")
        self.assertEqual(len(data), report["container_bytes"])
        self.assertEqual(files[0]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(container.name, "geometry--" + files[0]["sha256"] + ".vgp")
        os.utime(container, (1234567890, 1234567890))
        timestamp = container.stat().st_mtime_ns
        repeated = self.recipe('return cooker.virtual_geometry(cooker.primitive("box", {size={2,3,4}}), {})',
                               output="res://assets/runtime/model/vg/repeated.res")
        self.assertEqual(repeated["geometry_outputs"], files)
        self.assertEqual(repeated["virtual_geometry"]["build_key"], report["build_key"])
        self.assertEqual(container.stat().st_mtime_ns, timestamp)
        self.assertEqual(len(list(container.parent.glob("*.vgp"))), 1)
        self.assertEqual(result["vertices"], repeated["vertices"])

    def test_shared_geometry_batch_digests_and_corruption_between_batches(self):
        source = self.recipe('''return cooker.virtual_geometry(
    cooker.primitive("box", { size = { 2, 3, 4 } }), {}
)''', output="res://assets/runtime/model/shared/model.res")
        container_path = source["geometry_outputs"][0]["path"]
        container = self.project / container_path.removeprefix("res://")
        original = container.read_bytes()
        sha256 = hashlib.sha256(original).hexdigest()
        md5 = hashlib.md5(original).hexdigest()
        (self.project / "fragment.luau").write_text('''return cooker.scene({
    { asset = cooker.input("mesh") },
})''')
        pipeline = '''local steps = {}
for index = 0, 2 do
    table.insert(steps, {
        name = "part-" .. index,
        operation = "run-recipe",
        source = "res://fragment.luau",
        inputs = { mesh = "''' + source["output"] + '''" },
        external_render_resources = true,
        output = "res://assets/runtime/world/fragments/part-" .. index .. ".scn",
    })
end
return steps'''
        first = self.pipeline(pipeline)["steps"]
        self.assertEqual(len(first), 3)
        for report in first:
            self.assertEqual(report["input_sha256"][container_path], sha256)
            receipt_path = self.project / (report["output"].removeprefix("res://") + ".cook.json")
            receipt = json.loads(receipt_path.read_text())
            self.assertEqual(receipt["dependencies"][container_path], "md5:" + md5)
        self.assertTrue(all(step["skipped"] for step in self.pipeline(pipeline)["steps"]))
        # A completed batch releases its read pins. Same-length corruption with
        # the original timestamp must fail in the next batch, before publication.
        timestamp = container.stat()
        container.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        os.utime(container, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
        self.pipeline(pipeline, success=False)
        container.write_bytes(original)
        self.assertTrue(all(step["skipped"] for step in self.pipeline(pipeline)["steps"]))

    def test_minimum_visual_retains_virtual_geometry_and_publishes_its_container(self):
        original = self.recipe('''return cooker.virtual_geometry(
 cooker.primitive("box",{size={2,4,2}}),{})''',
                               output="res://assets/runtime/model/source/model.res")
        source = self.project / original["output"].removeprefix("res://")
        source_bytes = source.read_bytes()
        minimum = self.recipe('''return cooker.minimum_visual("visual",{
 triangle_ratio=.1,max_texture_size=128})''',
                              inputs={"visual": original["output"]},
                              output="res://assets/runtime/model/minimum/model.res")
        self.assertEqual(minimum["type"], "VirtualGeometryMesh")
        self.assertEqual(source.read_bytes(), source_bytes)
        containers = minimum["geometry_outputs"]
        self.assertEqual(len(containers), 1)
        container = self.project / containers[0]["path"].removeprefix("res://")
        original_container = self.project / original["geometry_outputs"][0]["path"].removeprefix("res://")
        self.assertEqual(container.parent, (self.project / "assets/runtime/model/minimum"))
        self.assertEqual(container.read_bytes(), original_container.read_bytes())

    def test_virtual_geometry_scene_inspection_uses_root_metadata(self):
        if REFERENCE is None:
            self.skipTest("MeshInstance scene fixtures need the matching private editor")
        built = self.recipe('return cooker.virtual_geometry(cooker.primitive("box", { size = { 2, 3, 4 } }), {})',
                            output="res://assets/runtime/model/inspection/model.res")
        root = self.job({"operation": "validate-resource", "source": built["output"]})
        (self.project / "inspect_source.gd").write_text('''extends SceneTree
func _initialize():
\tvar scene_root := Node3D.new()
\tscene_root.name = "InspectionScene"
\tvar geometry := load("res://assets/runtime/model/inspection/model.res")
\tfor index in 2:
\t\tvar instance := MeshInstance3D.new()
\t\tinstance.name = "Mesh" + str(index)
\t\tinstance.mesh = geometry
\t\tinstance.position.x = index * 4
\t\tscene_root.add_child(instance)
\t\tinstance.owner = scene_root
\tvar scene := PackedScene.new()
\tassert(scene.pack(scene_root) == OK)
\tassert(ResourceSaver.save(scene, "res://assets/runtime/model/inspection/scene.scn") == OK)
\tscene_root.free()
\tquit()
''')
        made = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                               "--script", "res://inspect_source.gd"], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=20)
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        self.assertNotIn("ERROR:", made.stdout + made.stderr)
        inspected = self.job({"operation": "validate-resource", "source": "res://assets/runtime/model/inspection/scene.scn",
                              "type": "PackedScene"})
        self.assertEqual(inspected["mesh_count"], 2)
        self.assertEqual(inspected["virtual_geometry_mesh_count"], 2)
        self.assertEqual(inspected["cluster_count"], root["cluster_count"] * 2)
        self.assertEqual(inspected["page_count"], root["page_count"] * 2)
        self.assertEqual(inspected["root_page_count"], root["root_page_count"] * 2)
        self.assertNotIn("vertex_count", inspected)

    def test_borrowed_virtual_geometry_root_publishes_its_container(self):
        built = self.recipe('return cooker.virtual_geometry(cooker.primitive("box", { size = { 2, 3, 4 } }), {})',
                            output="res://assets/runtime/model/original/model.res")
        copied = self.recipe('return cooker.input("mesh")', inputs={"mesh": built["output"]},
                             output="res://assets/runtime/model/copied/model.res")
        self.assertEqual(copied["virtual_geometry_builds"], 0)
        self.assertEqual(len(copied["geometry_outputs"]), 1)
        original_container = self.project / built["geometry_outputs"][0]["path"].removeprefix("res://")
        copied_container = self.project / copied["geometry_outputs"][0]["path"].removeprefix("res://")
        self.assertNotEqual(original_container.parent, copied_container.parent)
        self.assertEqual(original_container.read_bytes(), copied_container.read_bytes())
        for path in original_container.parent.iterdir():
            path.unlink()
        checked = self.job({"operation": "validate-resource", "source": copied["output"],
                            "type": "VirtualGeometryMesh"})
        self.assertGreater(checked["cluster_count"], 0)
        self.assertGreater(checked["root_page_count"], 0)

    def test_virtual_geometry_import_preserves_source_position_precision(self):
        side = 65
        positions = [(float(x), float(y), 5.0) for y in range(side) for x in range(side)]
        uvs = [(x / 64.0, y / 64.0) for y in range(side) for x in range(side)]
        indices = []
        for y in range(64):
            for x in range(64):
                a = y * side + x
                indices.extend((a, a + 1, a + side, a + 1, a + side + 1, a + side))
        # This valid face is smaller than the ordinary GPU vertex grid. The
        # hierarchy builder must receive source floats before that compression.
        first = len(positions)
        epsilon = 2.0 ** -20
        positions.extend(((0.0, 0.0, 5.0), (epsilon, 0.0, 5.0), (0.0, epsilon, 5.0)))
        uvs.extend(((0.0, 0.0), (1.0, 0.0), (0.0, 1.0)))
        indices.extend((first, first + 1, first + 2))
        payload = bytearray()
        views = []
        accessors = []

        def add(values, component_type, element_type, fmt, **extra):
            packed = b''.join(struct.pack(fmt, *value) for value in values)
            views.append({"buffer": 0, "byteOffset": len(payload), "byteLength": len(packed)})
            payload.extend(packed)
            accessors.append({"bufferView": len(views) - 1, "componentType": component_type,
                              "type": element_type, "count": len(values), **extra})
            return len(accessors) - 1

        vertex = add(positions, 5126, "VEC3", "<fff", min=[0, 0, 5], max=[64, 64, 5])
        normal = add([(0.0, 0.0, 1.0)] * len(positions), 5126, "VEC3", "<fff")
        uv = add(uvs, 5126, "VEC2", "<ff")
        index = add([(value,) for value in indices], 5125, "SCALAR", "<I")
        (self.project / "precision.bin").write_bytes(payload)
        (self.project / "precision.gltf").write_text(json.dumps({
            "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "name": "SourcePrecision"}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": vertex, "NORMAL": normal,
                                                       "TEXCOORD_0": uv}, "indices": index}]}],
            "buffers": [{"uri": "precision.bin", "byteLength": len(payload)}],
            "bufferViews": views, "accessors": accessors,
        }))
        result = self.job({"operation": "import-scene", "source": "res://precision.gltf",
                           "output": "res://assets/runtime/model/precision/scene.scn",
                           "options": {"meshes/generate_lods": False}})
        policy = result["virtual_geometry"]
        self.assertEqual(policy["converted_meshes"], 1)
        self.assertEqual(policy["nodes"][0]["representation"], "virtual_geometry")
        self.assertEqual(policy["nodes"][0]["build"]["source_triangles"], 8193)
        self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})

    def test_virtual_geometry_material_processing_preserves_root(self):
        if REFERENCE is None:
            self.skipTest("material slot inspection needs the matching private editor")
        source = self.recipe('return cooker.virtual_geometry(cooker.primitive("box", { size = { 2, 3, 4 } }), {})',
                             output="res://assets/runtime/model/original/model.res")
        material = self.project / "assets/runtime/material/red/red.tres"
        material.parent.mkdir(parents=True)
        material.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\nalbedo_color=Color(1,0,0,1)\n')
        source_path = self.project / source["output"].removeprefix("res://")
        before = source_path.read_bytes()
        job = {"operation": "process-mesh", "source": source["output"],
               "material_overrides": {"0": "res://assets/runtime/material/red/red.tres"},
               "output": "res://assets/runtime/model/processed/model.res"}
        processed = self.job(job)
        self.assertEqual(processed["type"], "VirtualGeometryMesh")
        self.assertGreater(processed["cluster_count"], 0)
        self.assertEqual(len(processed["geometry_outputs"]), 1)
        self.assertEqual(processed["geometry_outputs"][0]["sha256"], source["geometry_outputs"][0]["sha256"])
        self.assertNotIn("vertices", processed)
        self.assertEqual(source_path.read_bytes(), before)
        (self.project / "inspect_material.gd").write_text('''extends SceneTree
func _initialize():
\tvar mesh := load("res://assets/runtime/model/processed/model.res")
\tvar material: StandardMaterial3D = mesh.surface_get_material(0)
\tassert(material != null)
\tassert(material.albedo_color == Color(1, 0, 0, 1))
\tquit()
''')
        inspected = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                                    "--script", "res://inspect_material.gd"], capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=20)
        self.assertEqual(inspected.returncode, 0, inspected.stdout + inspected.stderr)
        self.assertNotIn("ERROR:", inspected.stdout + inspected.stderr)
        output = self.project / job["output"].removeprefix("res://")
        saved = output.read_bytes()
        for unsupported in ({"lightmap_uv": True}, {"base_lod": 1},
                            {"collision_output": "res://assets/runtime/model/processed/collision.scn"}):
            refused = self.job({**job, **unsupported}, success=False)
            self.assertEqual(refused["error"], 2)  # ERR_UNAVAILABLE: a source representation is required.
            self.assertEqual(output.read_bytes(), saved)
        for path in source_path.parent.iterdir():
            path.unlink()
        self.job({"operation": "validate-resource", "source": job["output"], "type": "VirtualGeometryMesh"})

    def test_virtual_geometry_navigation_requires_collision_source(self):
        source = self.recipe('return cooker.virtual_geometry(cooker.primitive("box", { size = { 2, 3, 4 } }), {})',
                             output="res://assets/runtime/model/navigation-source/model.res")
        refused = self.job({"operation": "bake-navigation", "source": source["output"],
                            "output": "res://assets/runtime/world/navigation/navigation.res"}, success=False)
        self.assertEqual(refused["error"], 2)  # ERR_UNAVAILABLE, without expanding virtual mesh arrays.
        self.assertFalse((self.project / "assets/runtime/world/navigation/navigation.res").exists())

    def test_virtual_geometry_publication_and_packed_range_reads(self):
        if REFERENCE is None:
            self.skipTest("packed range reads need the matching private editor")
        root_path = "res://assets/runtime/model/vg/model.res"
        (self.project / "build-vg.luau").write_text('return cooker.virtual_geometry(cooker.primitive("box", {size={2,3,4}}), {})')
        build_pipeline = 'return {{operation="run-recipe",source="res://build-vg.luau",output=' + json.dumps(root_path) + '}}'
        built = self.pipeline(build_pipeline)["steps"][0]
        self.assertTrue(self.pipeline(build_pipeline)["steps"][0]["skipped"])
        container_path = built["geometry_outputs"][0]["path"]
        container = self.project / container_path.removeprefix("res://")
        original = container.read_bytes()
        checked = self.job({"operation": "validate-resource", "source": root_path,
                            "type": "VirtualGeometryMesh"})
        self.assertEqual(checked["page_count"], built["virtual_geometry"]["pages"])
        self.assertFalse(checked["render_ready"])
        self.assertNotIn("vertex_count", checked)
        manifest_path = "res://assets/runtime/model/vg/asset.manifest.json"
        def publish_manifest(output=manifest_path, success=True):
            return self.pipeline('return {{operation="asset-manifest",output=' + json.dumps(output) +
                                 ',asset_id="vg",revision=1,kind="model",entry="model.res",files={"model.res"}}}', success)
        publish_manifest()
        self.assertTrue(publish_manifest()["steps"][0]["skipped"])
        manifest = json.loads((container.parent / "asset.manifest.json").read_text())
        self.assertEqual({row["path"] for row in manifest["files"]}, {"model.res", container.name})
        record = next(row["streaming"] for row in manifest["files"] if row["path"] == "model.res")
        self.assertEqual(record["resource_type"], "VirtualGeometryMesh")
        deferred = [row for row in record["dependencies"] if row.get("deferred")]
        self.assertEqual(len(deferred), 1)
        self.assertEqual(Path(deferred[0]["path"]).as_posix(), container.name)
        self.assertEqual(deferred[0]["md5"], hashlib.md5(original).hexdigest())
        self.assertEqual(deferred[0]["bytes"], len(original))
        graph = {Path(row["path"]).as_posix(): [Path(path).as_posix() for path in row["dependencies"]]
                 for row in record["dependency_graph"]}
        self.assertIn(container.name, graph["model.res"])
        self.assertEqual(graph[container.name], [])
        self.assertGreater(record["geometry_bytes"], 32768 * built["virtual_geometry"]["root_pages"])
        self.assertLess(record["geometry_bytes"], 100000)
        self.assertEqual(sum(row["geometry_bytes"] for row in record["costs"]), record["geometry_bytes"])
        pack_path = "res://content/releases/vg.pck"
        packed = self.job({"operation": "pack", "output": pack_path, "files": [root_path]})
        self.assertEqual(set(packed["files"]), {root_path, container_path})
        # Each refusal precedes publication; no broken manifest/pack is left.
        for payload in (original[:-1], bytes([original[0] ^ 1]) + original[1:]):
            container.write_bytes(payload)
            self.pipeline(build_pipeline, success=False)
            self.job({"operation": "validate-resource", "source": root_path}, success=False)
            publish_manifest("res://assets/runtime/model/vg/bad.json", success=False)
            self.assertFalse((container.parent / "bad.json").exists())
            self.job({"operation": "pack", "output": "res://content/releases/bad.pck", "files": [root_path]}, success=False)
            self.assertFalse((self.project / "content/releases/bad.pck").exists())
        container.unlink()
        self.pipeline(build_pipeline, success=False)
        missing = self.job({"operation": "validate-resource", "source": root_path}, success=False)
        self.assertEqual(missing["error"], 7)
        # Remove loose geometry. A fresh editor must obtain both files from PCK.
        (self.project / root_path.removeprefix("res://")).unlink()
        probe = self.project / "packed_pages.gd"
        probe.write_text('''extends SceneTree
func _initialize():
    assert(ProjectSettings.load_resource_pack("res://content/releases/vg.pck"))
    var mesh = load("res://assets/runtime/model/vg/model.res")
    assert(mesh is VirtualGeometryMesh)
    var data = mesh.get_geometry_data()
    var root: PackedByteArray = data.root
    var directory = root.decode_u64(168)
    var file = FileAccess.open("res://assets/runtime/model/vg/" + data.container, FileAccess.READ)
    assert(file != null and file.get_length() == data.container_bytes)
    var verified = 0
    for page in root.decode_u32(40):
        var location = directory + 32 + page * 40
        file.seek(root.decode_u64(location))
        var bytes = file.get_buffer(root.decode_u32(location + 8))
        assert(bytes.size() == root.decode_u32(location + 8))
        assert(bytes.decode_u32(0) == 0x47504756)
        verified += 1
    print("VG_PACKED_PAGES:", verified)
    quit()
''')
        loaded = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project), "--script", "res://packed_pages.gd"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        self.assertEqual(loaded.returncode, 0, loaded.stdout + loaded.stderr)
        self.assertNotIn("ERROR:", loaded.stdout + loaded.stderr)
        self.assertIn("VG_PACKED_PAGES:" + str(built["virtual_geometry"]["pages"]), loaded.stdout)

    def test_virtual_geometry_preserves_mixed_surface_materials_without_source_mesh_dependency(self):
        if REFERENCE is None:
            self.skipTest("resource round trip needs the matching private editor")
        directory = self.project / "assets/runtime/model/vg"
        directory.mkdir(parents=True)
        source = self.project / "make_source.gd"
        source.write_text('''extends SceneTree
func _initialize():
    var mesh = ArrayMesh.new()
    var red = StandardMaterial3D.new()
    red.albedo_color = Color(0.8,0.1,0.2)
    var green = StandardMaterial3D.new()
    green.albedo_color = Color(0.1,0.7,0.3)
    for surface in 3:
        var arrays = []
        arrays.resize(Mesh.ARRAY_MAX)
        var offset = Vector3(surface,0,0)
        arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([offset,offset+Vector3(0,1,0),offset+Vector3(1,0,0)])
        if surface != 1:
            arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3(0,0,1),Vector3(0,0,1),Vector3(0,0,1)])
        if surface == 0:
            arrays[Mesh.ARRAY_TANGENT] = PackedFloat32Array([1,0,0,1,1,0,0,1,1,0,0,1])
            arrays[Mesh.ARRAY_COLOR] = PackedColorArray([Color.RED,Color.GREEN,Color.BLUE])
            arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([Vector2.ZERO,Vector2(0,1),Vector2(1,0)])
        if surface == 1:
            arrays[Mesh.ARRAY_TEX_UV2] = PackedVector2Array([Vector2(2,3),Vector2(4,5),Vector2(6,7)])
        mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
        mesh.surface_set_material(surface, green if surface == 1 else red)
    assert(ResourceSaver.save(mesh,"res://assets/runtime/model/vg/source.res") == OK)
    quit()
''')
        made = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project), "--script", "res://make_source.gd"],
                              capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        self.assertNotIn("ERROR:", made.stdout + made.stderr)
        result = self.recipe('return cooker.virtual_geometry("source", {})',
                             inputs={"source": "res://assets/runtime/model/vg/source.res"},
                             output="res://assets/runtime/model/vg/model.res")
        report = result["virtual_geometry"]
        self.assertEqual(report["source_triangles"], 3)
        self.assertEqual(report["source_surfaces"], 3)
        self.assertEqual(report["uv_sets"], 2)
        self.assertTrue(report["has_tangents"])
        self.assertTrue(report["has_colors"])
        (directory / "source.res").unlink()
        inspect = self.project / "inspect.gd"
        inspect.write_text('''extends SceneTree
func _initialize():
    var path = "res://assets/runtime/model/vg/model.res"
    var mesh = load(path) as VirtualGeometryMesh
    assert(mesh != null and mesh.has_geometry())
    assert(mesh.get_surface_count() == 3)
    var red = mesh.surface_get_material(0) as StandardMaterial3D
    var green = mesh.surface_get_material(1) as StandardMaterial3D
    assert(is_equal_approx(red.albedo_color.r,0.8))
    assert(is_equal_approx(green.albedo_color.g,0.7))
    assert(mesh.surface_get_material(2) == red)
    assert(ResourceLoader.get_dependencies(path).is_empty())
    var data = mesh.get_geometry_data()
    var root_bytes: PackedByteArray = data.root
    assert(root_bytes.slice(0,4).get_string_from_ascii() == "VGMR")
    assert(root_bytes.decode_u32(44) == 3)
    assert(FileAccess.file_exists(path.get_base_dir().path_join(data.container)))
    print("VG_COOKER_MATERIALS: passed")
    quit()
''')
        checked = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project), "--script", "res://inspect.gd"],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("VG_COOKER_MATERIALS: passed", checked.stdout)
        self.assertNotIn("ERROR:", checked.stdout + checked.stderr)

    def test_virtual_geometry_refusals_do_not_publish_or_overwrite(self):
        for options in ("{unknown=true}", "{normal_bits=2.5}", "{position_precision=44}", "{preserve_area=1}"):
            output = f"res://assets/runtime/model/vg/rejected-{self.index}.res"
            self.recipe('return cooker.virtual_geometry(cooker.primitive("box", {}),' + options + ')', False,
                        output=output)
            self.assertFalse((self.project / output.removeprefix("res://")).exists())
        for expression in ('cooker.virtual_geometry(mesh,{})', 'cooker.scene({{asset=mesh}})',
                           'cooker.collision(mesh,"trimesh")', 'cooker.minimum_visual(mesh,{})',
                           '(function() error("after build") end)()',
                           '(function() pcall(function() cooker.virtual_geometry(mesh,{}) end); return cooker.primitive("box",{}) end)()'):
            output = f"res://assets/runtime/model/vg/rejected-{self.index}.res"
            self.recipe('local mesh=cooker.virtual_geometry(cooker.primitive("box",{}),{})\nreturn ' + expression,
                        False, output=output)
            self.assertFalse((self.project / output.removeprefix("res://")).exists())
        self.assertFalse(list(self.project.rglob("*.vgp")))
        result = self.recipe('return cooker.virtual_geometry(cooker.primitive("box",{}),{})',
                             output="res://assets/runtime/model/vg/first.res")
        record = result["geometry_outputs"][0]
        container = self.project / record["path"].removeprefix("res://")
        container.write_bytes(b"existing data must survive")
        failed = self.recipe('return cooker.virtual_geometry(cooker.primitive("box",{}),{})', False,
                             output="res://assets/runtime/model/vg/conflict.res")
        self.assertEqual(failed["error"], 32)  # ERR_ALREADY_EXISTS.
        self.assertEqual(container.read_bytes(), b"existing data must survive")
        self.assertFalse((container.parent / "conflict.res").exists())
        self.assertFalse(list(self.project.rglob("*.partial*")))

    def test_primitives_material_and_mesh(self):
        for code, kind in [
            ('return cooker.primitive("sphere", {radius=2,segments=12,rings=6})', "SphereMesh"),
            ('return cooker.primitive("cylinder", {height=3,radius=1,top_radius=0})', "CylinderMesh"),
            ('return cooker.material({color={0.3,0.4,0.5},metallic=0.8,roughness=0.4})', "StandardMaterial3D"),
            ('return cooker.mesh({vertices={{0,0,0},{1,0,0},{0,0,1}},indices={1,2,3},uvs={{0,0},{1,0},{0,1}}})', "ArrayMesh"),
        ]:
            with self.subTest(kind=kind):
                asset_kind = "material" if kind == "StandardMaterial3D" else "model"
                result = self.recipe(code, output=f"res://assets/runtime/{asset_kind}/basic/result-{self.index}.res")
                self.assertEqual(result["type"], kind)
                self.job({"operation": "validate-resource", "source": result["output"], "type": kind})

    def test_procedural_textures_are_seeded_and_effects_are_typed(self):
        texture_code = '''return cooker.procedural_texture({kind="soft-noise",size=64,seed=17,
 color={0.8,0.95,1,0.8},secondary={0.1,0.2,0.4,0},noise_scale=6})'''
        first = self.recipe(texture_code, output="res://content/worlds/fixture/generated/noise-a.res")
        second = self.recipe(texture_code, output="res://content/worlds/fixture/generated/noise-b.res")
        self.assertEqual(first["type"], "PortableCompressedTexture2D")
        self.assertEqual(second["type"], "PortableCompressedTexture2D")
        self.assertEqual(first["seed"], second["seed"])

        effect_code = '''local spark=cooker.procedural_texture({kind="spark",size=64,color={1,0.78,0.3,1}})
local ring=cooker.procedural_texture({kind="ring",size=64,radius=0.31,width=0.08,color={0.3,0.9,1,0.9}})
local mesh=cooker.primitive("sphere",{radius=0.12,segments=8,rings=4})
return cooker.effect({name="TypedEffect",mode="one_shot",duration=1.2,layers={
 {type="billboard_particles",name="Sparks",texture=spark,amount=96,lifetime=0.8,
  size={0.12,0.3},direction={0,1,0},spread=48,velocity={0.6,2.4},gravity={0,-1.2,0},
  damping={0.1,0.8},emission_shape="sphere_surface",emission_radius=0.3,
  color_gradient={{0,{1,0.88,0.45,1}},{0.7,{0.3,0.9,1,0.8}},{1,{0.1,0.3,0.8,0}}},
  alpha_curve={{0,0},{0.15,1},{1,0}},scale_curve={{0,0.2},{0.25,1.1},{1,0.1}},
  visibility_aabb={position={-2,-1,-2},size={4,4,4}}},
 {type="animated_sprite",name="Ring",texture=ring,size={1.6,1.6},blend="add",
  start_scale=0.2,peak_scale=1,end_scale=1.5,start_alpha=0,peak_alpha=0.9,end_alpha=0},
 {type="simple_mesh",name="Core",mesh=mesh,color={1,0.7,0.25,0.75},blend="add",scale={1,1,1}}
}})'''
        result = self.recipe(effect_code, seed=99, output="res://content/worlds/fixture/generated/typed-effect.scn")
        self.assertEqual(result["type"], "PackedScene")
        self.assertEqual(result["effect"], {"layers": 3, "particle_systems": 1,
                                            "declared_particles": 96, "textures": 2})
        report = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(report["node_count"], 5)
        self.assertEqual(report["effect_texture_count"], 1)
        self.assertGreater(report["effect_texture_bytes"], 0)
        self.assertNotIn("effect_texture_missing", report)

        # A repair round with identical Luau, parameters and seed must produce the
        # exact same artifact bytes. Keep the source and output paths stable so
        # this checks the Cooker rather than path-dependent serialization.
        deterministic_source = self.project / "deterministic-effect.luau"
        deterministic_source.write_text(effect_code)
        deterministic_output = "res://content/worlds/fixture/generated/deterministic-effect.scn"
        deterministic_job = {
            "operation": "run-recipe", "source": "res://deterministic-effect.luau",
            "output": deterministic_output, "seed": 99,
        }
        deterministic_first = self.job(deterministic_job)
        deterministic_path = self.project / deterministic_output.removeprefix("res://")
        deterministic_bytes = deterministic_path.read_bytes()
        deterministic_path.unlink()
        deterministic_second = self.job(deterministic_job)
        self.assertEqual(deterministic_first["sha256"], deterministic_second["sha256"])
        self.assertEqual(deterministic_path.read_bytes(), deterministic_bytes)

    def test_texture_inputs_and_effect_limits(self):
        texture = self.recipe('return cooker.procedural_texture({kind="radial",size=32})',
                              output="res://content/worlds/fixture/generated/input-texture.res")
        effect = self.recipe('''return cooker.effect({mode="loop",duration=4,layers={{
 type="billboard_particles",texture=cooker.input("particle"),amount=32,lifetime=8,
 emission_shape="box",emission_extents={2,0.5,2},velocity={0.1,0.4}}}})''',
                             inputs={"particle": texture["output"]},
                             output="res://content/worlds/fixture/generated/input-effect.scn")
        self.assertIn(texture["output"], effect["input_sha256"])
        rejected = [
            'return cooker.procedural_texture({kind="radial",size=48})',
            'return cooker.procedural_texture({kind="shader",size=64})',
            'local t=cooker.procedural_texture({kind="radial"}); return cooker.effect({layers={{type="billboard_particles",texture=t,amount=513}}})',
            'local t=cooker.procedural_texture({kind="radial"}); return cooker.effect({layers={{type="billboard_particles",texture=t,amount=512},{type="billboard_particles",texture=t,amount=512},{type="billboard_particles",texture=t,amount=1}}})',
            'local t=cooker.procedural_texture({kind="radial"}); return cooker.effect({mode="one_shot",layers={{type="billboard_particles",texture=t,lifetime=5.1}}})',
            'local t=cooker.procedural_texture({kind="radial"}); return cooker.effect({layers={{type="script",texture=t}}})',
            'local t=cooker.procedural_texture({kind="radial"}); return cooker.effect({layers={{type="animated_sprite",texture=t,shader="x"}}})',
        ]
        for code in rejected:
            with self.subTest(code=code[:70]):
                self.recipe(code, success=False, output=f"res://content/worlds/fixture/generated/rejected-{self.index}.scn")

    def test_material_texture_slots_and_rejects(self):
        albedo = self.recipe('return cooker.procedural_texture({kind="radial",size=32,color={0.8,0.4,0.2,1}})',
                             output="res://content/worlds/fixture/generated/mat-albedo.res")
        orm = self.recipe('return cooker.procedural_texture({kind="soft-noise",size=32,seed=3})',
                          output="res://content/worlds/fixture/generated/mat-orm.res")
        result = self.recipe('''return cooker.material({color={1,1,1},roughness=1,metallic=1,
 albedo_texture=cooker.input("albedo"),orm_texture=cooker.input("orm"),uv_scale=2,triplanar=true})''',
                             inputs={"albedo": albedo["output"], "orm": orm["output"]},
                             output="res://content/worlds/fixture/generated/pbr-material.res")
        self.assertEqual(result["type"], "StandardMaterial3D")
        self.assertIn(albedo["output"], result["input_sha256"])
        self.assertIn(orm["output"], result["input_sha256"])
        scalar = self.recipe('return cooker.material({color={0.3,0.4,0.5},metallic=0.8,roughness=0.4})')
        self.assertEqual(scalar["type"], "StandardMaterial3D")
        self.recipe('return cooker.material({albedo_texture=cooker.primitive("box",{})})', success=False,
                    output="res://content/worlds/fixture/generated/bad-material-mesh.res")
        self.recipe('return cooker.material({transparency="shader"})', success=False,
                    output="res://content/worlds/fixture/generated/bad-material-mode.res")
        self.recipe('return cooker.material({uv_scale=0})', success=False,
                    output="res://content/worlds/fixture/generated/bad-material-uv.res")

    def test_parameters_rng_bounds_and_independent_jobs(self):
        code = '''local b = cooker.primitive("box", {size=cooker.parameters.size})
local a = cooker.bounds(b)
assert(a.min[1] == -2 and a.max[3] == 4)
assert(cooker.random() == 270369 / 4294967296)
assert(cooker.random() == 67634689 / 4294967296)
return b'''
        for _ in range(2):
            self.recipe(code, parameters={"size": [4, 6, 8]}, seed=1)
        self.recipe('assert(cooker.parameters.option == true); ' + BOX, parameters={"option": True})

    def test_bridge_recipe_is_parameterized(self):
        code = (ROOT / "cooker/examples/bridge.luau").read_text()
        for segments in (8, 24):
            result = self.recipe(code, output=f"res://content/worlds/fixture/generated/bridge-{segments}.scn",
                                 parameters={"span": 8, "width": 2.6, "rise": 1.25, "segments": segments})
            self.assertEqual(result["type"], "PackedScene")
            self.assertGreater(result["instances"], segments * 5)
            report = self.job({"operation": "validate-resource", "source": result["output"]})
            self.assertEqual(report["node_count"], 5)  # Root + four mesh/material batches.

    def test_declared_input_mesh_material_and_dependency_hashes(self):
        mesh = self.recipe()
        material = self.recipe('return cooker.material({roughness=0.4})')
        result = self.recipe('''local m = cooker.input("shape")
assert(cooker.bounds(m).max[2] == 1.5)
return cooker.scene({{asset=m,material=cooker.input("finish")}})''',
            inputs={"shape": mesh["output"], "finish": material["output"]},
            output="res://content/worlds/fixture/generated/from-input.scn")
        self.assertEqual(set(result["input_sha256"]), {mesh["output"], material["output"]})
        pack = self.job({"operation": "pack", "files": [result["output"]], "output": "res://content/releases/inputs.pck"})
        self.assertEqual(set(pack["files"]), {mesh["output"], material["output"], result["output"]})

    def test_field_rasters_points_and_sampling(self):
        field = '''local data={}
for z=0,2 do for x=0,3 do table.insert(data,x+z*10); table.insert(data,-x) end end
local f=cooker.field({channels={"height","water_distance"},width=4,height=3,origin={-2,5},cell=2,data=data,labels={"meadow","wetland"}})
local a,b=cooker.sample(f,-2+1,5+2)
assert(math.abs(a-10.5)<1e-5 and math.abs(b+0.5)<1e-5, "bilinear "..a.." "..b)
local c=cooker.sample(f,-100,100)
assert(c==20, "clamped "..c)
local info=cooker.field_info(f)
assert(info.width==4 and info.height==3 and info.cell==2 and info.origin[2]==5 and info.channels[2]=="water_distance" and info.labels[1]=="meadow")
return f'''
        target = self.project / "content/worlds/fixture/generated/field/a.res"
        repeated = self.recipe(field, output="res://content/worlds/fixture/generated/field/a.res")
        target.unlink()
        first = self.recipe(field, output="res://content/worlds/fixture/generated/field/a.res")
        # Same Luau, seed and output path give byte-identical rasters.
        self.assertEqual(first["sha256"], repeated["sha256"])
        self.assertEqual(first["type"], "Image")
        self.assertEqual(first["field_cells"], 24)
        report = self.job({"operation": "validate-resource", "source": first["output"], "type": "Image"})
        self.assertEqual(report["field"]["kind"], "field")
        self.assertEqual(report["field"]["channels"], ["height", "water_distance"])
        # A downstream recipe reads the cooked field as a declared input.
        reader = self.recipe('''local f=cooker.input("site")
local h=cooker.sample(f,0,7)
assert(math.abs(h-11)<1e-5, tostring(h))
return cooker.points({kinds={"perch","rest"},labels={"wetland"},points={
 {kind=1,habitat=1,position={0,h,7},normal={0,2,0},radius=.4},{kind=2,position={1,0,1}}}})''',
            inputs={"site": first["output"]}, output="res://content/worlds/fixture/generated/field/anchors.res")
        self.assertEqual(reader["points"], 2)
        report = self.job({"operation": "validate-resource", "source": reader["output"], "type": "Image"})
        self.assertEqual(report["field"]["kind"], "points")
        self.assertEqual(report["height"], 2)
        self.assertEqual(report["field"]["count"], 2)
        empty = self.recipe('return cooker.points({kinds={"a"},points={}})',
                            output="res://content/worlds/fixture/generated/field/empty.res")
        self.assertEqual(empty["points"], 0)
        report = self.job({"operation": "validate-resource", "source": empty["output"], "type": "Image"})
        self.assertEqual(report["field"]["count"], 0)
        for code in (
            'return cooker.field({channels={"h"},width=2,height=2,data={1,2,3}})',
            'return cooker.field({channels={"h","h"},width=2,height=2,data={1,2,3,4,5,6,7,8}})',
            'return cooker.field({channels={"a","b","c","d","e"},width=2,height=2,data={}})',
            'return cooker.field({channels={"bad name"},width=2,height=2,data={1,2,3,4}})',
            'return cooker.field({channels={"h"},width=1,height=2,data={1,2}})',
            'return cooker.field({channels={"h"},width=2,height=2,data={1,2,3,0/0}})',
            'return cooker.field({channels={"h"},width=2,height=2,data={1,2,3,4},extra=1})',
            'cooker.sample(cooker.primitive("box",{}),0,0); return cooker.primitive("box",{})',
            'return cooker.points({kinds={"a"},points={{kind=2,position={0,0,0}}}})',
            'return cooker.points({kinds={"a"},points={{kind=1,habitat=1,position={0,0,0}}}})',
            'return cooker.points({kinds={"a"},points={{kind=1,position={0,0,0},normal={0,0,0}}}})',
        ):
            with self.subTest(code=code[:60]):
                self.recipe(code, success=False)

    def test_declared_input_count_limit(self):
        mesh = self.recipe()
        many = {f"m{i}": mesh["output"] for i in range(64)}
        self.recipe('return cooker.scene({{asset=cooker.input("m63")}})', inputs=many,
                    output="res://content/worlds/fixture/generated/inputs/64.scn")
        many["m64"] = mesh["output"]
        self.recipe('return cooker.primitive("box",{})', inputs=many, success=False)

    def test_children_and_parts_expose_cooked_layouts(self):
        model = self.recipe('''local b=cooker.primitive("box",{size={2,4,2}})
local m=cooker.material({roughness=.5})
return cooker.scene({{asset=b,material=m,lightmap=true,name="Body",position={0,2,0}}})''',
                            output="res://content/worlds/fixture/generated/parts/model.scn")
        layout = self.recipe('''local m=cooker.input("model")
return cooker.scene({{asset=m,name="Tree_a_0001",position={10,0,0},scale={2,2,2}},{asset=m,name="Tree_b_0002",position={0,0,5}}})''',
                             inputs={"model": model["output"]}, output="res://content/worlds/fixture/generated/parts/layout.scn")
        self.recipe('''local list=cooker.children(cooker.input("layout"))
assert(#list==2 and list[1].name=="Tree_a_0001", "children")
assert(list[1].position[1]==10 and list[1].basis[2][2]==2, "transform")
assert(math.abs(list[1].max[2]-8)<1e-4 and math.abs(list[1].min[1]-8)<1e-4, "bounds "..list[1].max[2])
local parts=cooker.parts(cooker.input("model"))
assert(#parts==1 and parts[1].material~=nil and parts[1].position[2]==2, "parts")
local pieces={}
for i=1,3 do table.insert(pieces,{asset=parts[1].mesh,material=parts[1].material,position={i,0,0}}) end
return cooker.scene(pieces)''', inputs={"layout": layout["output"], "model": model["output"]},
                    output="res://content/worlds/fixture/generated/parts/batched.scn")
        clump = self.recipe('''local b=cooker.primitive("box",{size={.1,1,.1}})
return cooker.scene({{asset=b,position={0,.5,0}},{asset=b,position={.3,.5,0}},{asset=b,position={0,.5,.3}}})''',
                            output="res://content/worlds/fixture/generated/parts/clump.scn")
        self.recipe('''local parts=cooker.parts(cooker.input("clump"))
assert(#parts==3 and math.abs(parts[2].position[1]-.3)<1e-5 and parts[2].material==nil, "multimesh parts "..#parts)
return cooker.scene({{asset=parts[1].mesh}})''', inputs={"clump": clump["output"]},
                    output="res://content/worlds/fixture/generated/parts/clump-batch.scn")
        for code in ('return cooker.scene({{asset=cooker.parts(cooker.primitive("box",{}))[1].mesh}})',
                     'cooker.children(cooker.primitive("box",{})); return cooker.primitive("box",)'):
            with self.subTest(code=code[:50]):
                self.recipe(code, success=False)

    def test_packed_scene_bounds_removal_and_luau_composition(self):
        base = self.recipe('local b=cooker.primitive("box",{size={8,1,8}}); return cooker.scene({{asset=b}})',
                           output="res://content/worlds/fixture/generated/base.scn")
        model = self.recipe('local b=cooker.primitive("box",{size={2,4,2}}); return cooker.scene({{asset=b}})',
                            output="res://content/worlds/fixture/generated/model.scn")
        result = self.recipe('''local base=cooker.input("base")
local model=cooker.input("model")
local bounds=cooker.bounds(model)
local height=bounds.max[2]-bounds.min[2]
assert(math.abs(height-4) < 0.001, "unexpected scene height " .. tostring(height))
local scale=8/height
return cooker.scene({
 {asset=base,name="Base",remove={"Batch0"}},
 {asset=model,name="Hero",position={3,4,5},scale={scale,scale,scale}}
})''', inputs={"base": base["output"], "model": model["output"]},
            output="res://content/worlds/fixture/generated/composed.scn")
        self.assertEqual(result["type"], "PackedScene")
        report = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(report["node_count"], 4)
        self.recipe('return cooker.scene({{asset=cooker.input("base"),remove={"Missing"}}})',
                    inputs={"base": base["output"]}, output="res://content/worlds/fixture/generated/bad-removal.scn", success=False)

    def test_selected_children_keep_transforms_and_reject_invalid_names(self):
        source = self.recipe('''local mesh=cooker.primitive("box",{size={2,4,2}})
return cooker.scene({{asset=mesh,lightmap=true,name="First",position={-64,2,0},scale={2,1,3}},
 {asset=mesh,lightmap=true,name="Second",position={64,2,0}}})''',
                             output="res://content/worlds/fixture/generated/select/source.scn")
        selected = self.recipe('''local source=cooker.input("visual")
local names=cooker.child_names(source)
assert(#names==2 and names[1]=="First" and names[2]=="Second")
return cooker.scene({{asset=source,keep={names[1]}}})''',
                               inputs={"visual": source["output"]},
                               output="res://content/worlds/fixture/generated/select/first.scn")
        self.recipe('''local box=cooker.bounds(cooker.input("visual"))
assert(box.min[1]==-66 and box.max[1]==-62 and box.max[3]==3)
return cooker.input("visual")''', inputs={"visual": selected["output"]},
                    output="res://content/worlds/fixture/generated/select/verified.scn")
        for keep in ('{}', '{"Missing"}', '{"First","First"}', '{"../First"}'):
            self.recipe('return cooker.scene({{asset=cooker.input("visual"),keep='+keep+'}})',
                        inputs={"visual": source["output"]}, success=False)

    def test_selected_nested_scenes_do_not_reinstantiate_owned_children(self):
        leaf = self.recipe('''local mesh=cooker.primitive("box",{})
return cooker.scene({{asset=mesh,lightmap=true,name="Leaf"}})''',
                           output="res://content/worlds/fixture/generated/nested/leaf.scn")
        branch = self.recipe('return cooker.scene({{asset=cooker.input("leaf"),name="Branch"}})',
                             inputs={"leaf": leaf["output"]},
                             output="res://content/worlds/fixture/generated/nested/branch.scn")
        selected = self.recipe('return cooker.scene({{asset=cooker.input("branch"),keep={"Branch"}}})',
                               inputs={"branch": branch["output"]},
                               output="res://content/worlds/fixture/generated/nested/selected.scn")
        report = self.job({"operation": "validate-resource", "source": selected["output"], "type": "PackedScene"})
        self.assertEqual(report["node_count"], 4)

    def test_static_fragments_share_render_dependencies(self):
        original = self.recipe('local mesh=cooker.primitive("box",{}); return cooker.scene({{asset=mesh}})',
                               output="res://content/worlds/fixture/generated/original.scn")
        outputs = []
        for index in range(2):
            result = self.recipe('return cooker.scene({{asset=cooker.input("visual"),keep={"Batch0"}}})',
                                 inputs={"visual": original["output"]}, external_render_resources=True,
                                 output=f"res://content/worlds/fixture/generated/fragments/part_{index}.scn")
            outputs.append(result["render_outputs"])
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(len(outputs[0]), 1)
        manifest = self.pipeline('''return {{operation="asset-manifest",
 output="res://content/worlds/fixture/generated/fragments/asset.manifest.json",asset_id="shared-fragments",
 revision=1,kind="world",entry="part_0.scn",files={"part_0.scn","part_1.scn"},include_render_dependencies=true}}
''')["steps"][0]
        self.assertEqual(len(manifest["files"]), 3)
        for row in manifest["files"]:
            if row["path"].endswith(".scn"):
                self.assertTrue(any("render-resources/" in entry["path"] for entry in row["streaming"]["dependencies"]))
                graph = row["streaming"]["dependency_graph"]
                self.assertEqual(row["streaming"]["dependency_graph_version"], 1)
                self.assertEqual({entry["path"] for entry in graph},
                                 {entry["path"] for entry in row["streaming"]["dependencies"]})
                self.assertTrue(any(entry["dependencies"] == [] for entry in graph))

    def test_recipe_created_render_dependencies_have_stable_byte_identity(self):
        source = '''local mesh=cooker.primitive("box",{})
local material=cooker.material({color={0.2,0.4,0.6}})
return cooker.scene({{asset=mesh,material=material}})'''
        outputs = []
        for index in range(2):
            result = self.recipe(source, external_render_resources=True,
                                 output=f"res://content/worlds/fixture/generated/anonymous/part_{index}.scn")
            outputs.append(sorted(result["render_outputs"]))
            checked = self.job({"operation": "validate-resource", "source": result["output"],
                                "type": "PackedScene"})
            self.assertEqual(checked["node_count"], 2)
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(len(outputs[0]), 2)

    def test_heightmap_preserves_grid_and_triangle_diagonal(self):
        source = self.recipe('''local vertices,indices={},{}
for z=0,4 do
 for x=0,4 do table.insert(vertices,{(x-2)*1.25,x*z*.5,(z-2)*1.25}) end
end
for z=0,3 do
 for x=0,3 do
  local a=z*5+x+1
  for _,i in {a,a+1,a+6,a,a+6,a+5} do table.insert(indices,i) end
 end
end
return cooker.mesh({vertices=vertices,indices=indices})''',
                             output="res://content/worlds/fixture/assets/runtime/terrain/test/grid.res")
        heightmap = self.recipe('''return cooker.collision(cooker.input("mesh"),"heightmap",{
 width=5,cell_size=1.25,max_error=.2})''',
                                inputs={"mesh": source["output"]},
                                output="res://content/worlds/fixture/assets/runtime/terrain/test/support.res")
        self.assertIsNotNone(REFERENCE)
        with (self.project / "project.godot").open("a") as project:
            project.write("config/disable_project_settings_override=false\n")
        (self.project / "inspect_heightmap.gd").write_text('''extends SceneTree
func _initialize():
    call_deferred("inspect")
func inspect():
    var shape = load("'''+heightmap["output"]+'''") as HeightMapShape3D
    var original = load("'''+source["output"]+'''") as Mesh
    var valid = shape != null and shape.map_width == 5 and shape.map_depth == 5
    valid = valid and shape.map_data.size() == 25
    valid = valid and shape.get_meta("veya_collision_support").quarter_turn
    valid = valid and shape.get_meta("veya_collision_support").achieved_error_metres == 0
    var body = StaticBody3D.new()
    var node = CollisionShape3D.new()
    node.shape = shape
    node.rotation.y = PI/2
    node.scale = Vector3(1.25,1,1.25)
    body.add_child(node)
    root.add_child(body)
    var control = StaticBody3D.new()
    var control_shape = CollisionShape3D.new()
    control_shape.shape = original.create_trimesh_shape()
    control.add_child(control_shape)
    control.position.x = 15
    root.add_child(control)
    await physics_frame
    await physics_frame
    var space = root.world_3d.direct_space_state
    for p in [Vector2(-1.9,-2.1),Vector2(-.3,.4),Vector2(.2,-.5),Vector2(1.8,1.6)]:
        var a = space.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(p.x,20,p.y),Vector3(p.x,-20,p.y)))
        var b = space.intersect_ray(PhysicsRayQueryParameters3D.create(Vector3(p.x+15,20,p.y),Vector3(p.x+15,-20,p.y)))
        valid = valid and not a.is_empty() and not b.is_empty()
        if not a.is_empty() and not b.is_empty():
            valid = valid and abs(a.position.y-b.position.y)<.0001
    body.free()
    control.free()
    print("HEIGHTMAP_SURFACE:",valid)
    quit(0 if valid else 1)
''')
        checked = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                                  "--script", "res://inspect_heightmap.gd"],
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout+checked.stderr)
        self.assertIn("HEIGHTMAP_SURFACE:true", checked.stdout)
        self.recipe('''return cooker.collision(cooker.input("mesh"),"heightmap",{
 width=4,cell_size=1.25,max_error=.2})''', success=False,
                    inputs={"mesh": source["output"]},
                    output="res://content/worlds/fixture/assets/runtime/terrain/test/invalid.res")

    def test_minimum_preserves_precomputed_lod_capability(self):
        source = self.recipe(
            'return cooker.primitive("sphere",{radius=2,segments=24,rings=12})',
            output="res://content/worlds/fixture/assets/runtime/model/lod/source.res")
        processed = self.job({
            "operation": "process-mesh", "source": source["output"],
            "output": "res://content/worlds/fixture/assets/runtime/model/lod/processed.res"})
        layout = self.recipe('''local mesh=cooker.input("mesh")
return cooker.scene({
 {asset=mesh,position={0,2,0},scale={1,2,.5}},
 {asset=mesh,position={7,2,-4},scale={2,2,.5}},
 {asset=mesh,position={14,2,-4},scale={3,2,.5}},
})''', inputs={"mesh": processed["output"]},
                             output="res://content/worlds/fixture/assets/runtime/model/lod/layout.scn")
        minimum = self.recipe('''return cooker.minimum_visual("visual",{
 triangle_ratio=1,max_texture_size=32})''',
                              inputs={"visual": layout["output"]},
                              output="res://content/worlds/fixture/assets/runtime/model/lod/minimum.scn")
        self.assertIsNotNone(REFERENCE, "LOD resource round trip needs --reference-godot")
        script = self.project / "inspect_minimum_lods.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var original = load("'''+layout["output"]+'''").instantiate()
    var minimum = load("'''+minimum["output"]+'''").instantiate()
    var before = original.get_child(0).multimesh as MultiMesh
    var after = minimum.get_child(0).multimesh as MultiMesh
    assert(before.instance_count == 3 and after.instance_count == 3)
    for index in 3:
        assert(before.get_instance_transform(index) == after.get_instance_transform(index))
    var source_lods = ImporterMesh.from_mesh(before.mesh)
    var minimum_lods = ImporterMesh.from_mesh(after.mesh)
    assert(source_lods.get_surface_lod_count(0) > 0)
    assert(minimum_lods.get_surface_lod_count(0) > 0)
    var arrays = after.mesh.surface_get_arrays(0)
    var vertex_count = arrays[Mesh.ARRAY_VERTEX].size()
    var base_count = arrays[Mesh.ARRAY_INDEX].size()
    for lod in minimum_lods.get_surface_lod_count(0):
        var indices = minimum_lods.get_surface_lod_indices(0, lod)
        assert(indices.size() >= 3 and indices.size() < base_count)
        assert(indices.size() % 3 == 0)
        for index in indices:
            assert(index >= 0 and index < vertex_count)
    original.free()
    minimum.free()
    print("MINIMUM_PRECOMPUTED_LODS:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_minimum_lods.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("MINIMUM_PRECOMPUTED_LODS:true", checked.stdout)

    def test_covered_lods_keep_disconnected_leaves_and_calibrated_distances(self):
        source = self.recipe('''local vertices, indices = {}, {}
for leaf = 0, 15 do
    local first = #vertices
    for y = 0, 4 do
        for x = 0, 4 do
            table.insert(vertices, {leaf % 4 * 2 + x / 4,
                math.floor(leaf / 4) * 2 + y / 4, leaf % 2 * .3})
        end
    end
    for y = 0, 3 do
        for x = 0, 3 do
            local a = first + y * 5 + x + 1
            for _, index in {a,a+1,a+6,a,a+6,a+5} do
                table.insert(indices,index)
            end
        end
    end
end
return cooker.mesh({vertices=vertices,indices=indices})''',
                             output="res://assets/runtime/model/leaves/source.res")
        covered = self.recipe('''return cooker.minimum_visual("mesh",{
 triangle_ratio=1,max_texture_size=32,lod_triangle_ratios={.5,.2,.03},
 lod_screen_errors={.2,.4,.8},lod_minimum_coverage=.99})''',
                              inputs={"mesh": source["output"]},
                              output="res://assets/runtime/model/leaves/covered.res")
        minimum = self.recipe('''return cooker.minimum_visual("mesh",{
 triangle_ratio=.05,max_texture_size=16,lod_triangle_ratios={.01}})''',
                              inputs={"mesh": covered["output"]},
                              output="res://assets/runtime/model/leaves/minimum.res")
        self.assertIsNotNone(REFERENCE, "LOD readback requires the Veya editor")
        script = self.project / "inspect_covered_lods.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var source = load("'''+source["output"]+'''")
    var covered = load("'''+covered["output"]+'''")
    var minimum = load("'''+minimum["output"]+'''")
    var first = ImporterMesh.from_mesh(covered)
    var last = ImporterMesh.from_mesh(minimum)
    assert(first.get_surface_lod_count(0) > 0)
    assert(first.get_surface_lod_count(0) == last.get_surface_lod_count(0))
    var points = covered.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
    var final_points = minimum.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
    var base = covered.surface_get_arrays(0)[Mesh.ARRAY_INDEX]
    var final_base = minimum.surface_get_arrays(0)[Mesh.ARRAY_INDEX]
    assert(base.size() == source.surface_get_array_index_len(0))
    assert(final_base.size() == base.size())
    for i in base.size():
        assert(points[base[i]].is_equal_approx(final_points[final_base[i]]))
    for level in first.get_surface_lod_count(0):
        var indices = first.get_surface_lod_indices(0,level)
        var final_indices = last.get_surface_lod_indices(0,level)
        assert(indices.size() < base.size() and indices.size() == final_indices.size())
        var distance = first.get_surface_lod_size(0,level)
        assert(abs(distance-.2)<.00001 or abs(distance-.4)<.00001 or abs(distance-.8)<.00001)
        assert(is_equal_approx(distance,last.get_surface_lod_size(0,level)))
        var represented = {}
        for i in range(0,indices.size(),3):
            var a = points[indices[i]]
            var b = points[indices[i+1]]
            var c = points[indices[i+2]]
            # A triangle cannot bridge separate authored leaves.
            assert(max(a.x,max(b.x,c.x))-min(a.x,min(b.x,c.x))<=1.001)
            assert(max(a.y,max(b.y,c.y))-min(a.y,min(b.y,c.y))<=1.001)
            represented[Vector2i(int(a.x/2),int(a.y/2))] = true
        # Interior leaves may thin. The four outer corners must remain.
        for corner in [Vector2i(0,0),Vector2i(3,0),Vector2i(0,3),Vector2i(3,3)]:
            assert(represented.has(corner),str(represented.keys()))
        for i in indices.size():
            assert(points[indices[i]].is_equal_approx(final_points[final_indices[i]]))
    print("COVERED_LODS:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_covered_lods.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("COVERED_LODS:true", checked.stdout, checked.stderr)

    def test_covered_lods_keep_ground_connected_trunk(self):
        source = self.recipe('''local vertices,indices={},{}
for y=0,24 do
    for side=0,7 do
        local angle=side*math.pi/4
        table.insert(vertices,{math.cos(angle)*.35,y*4/24,math.sin(angle)*.35})
    end
end
for y=0,23 do
    for side=0,7 do
        local a=y*8+side+1
        local b=y*8+(side+1)%8+1
        for _,i in {a,b,b+8,a,b+8,a+8} do table.insert(indices,i) end
    end
end
for branch=1,24 do
    local first=#vertices
    local y=branch*15/24+3
    table.insert(vertices,{2,y,0})
    table.insert(vertices,{2.2,y,0})
    table.insert(vertices,{2,y+.2,0})
    for _,i in {first+1,first+2,first+3} do table.insert(indices,i) end
end
return cooker.mesh({vertices=vertices,indices=indices})''',
                             output="res://assets/runtime/model/trunk/source.res")
        covered = self.recipe('''return cooker.minimum_visual("mesh",{
 triangle_ratio=1,lod_triangle_ratios={.5,.15,.05},
 lod_screen_errors={.5,1,2},lod_minimum_coverage=.5,lod_root_surfaces={1}})''',
                              inputs={"mesh": source["output"]},
                              output="res://assets/runtime/model/trunk/covered.res")
        self.assertIsNotNone(REFERENCE)
        (self.project / "inspect_trunk.gd").write_text('''extends SceneTree
func _initialize():
    var mesh=load("'''+covered["output"]+'''")
    var imported=ImporterMesh.from_mesh(mesh)
    var points=mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
    assert(imported.get_surface_lod_count(0)>0)
    for level in imported.get_surface_lod_count(0):
        var indices=imported.get_surface_lod_indices(0,level)
        assert(indices.size()<mesh.surface_get_array_index_len(0)*.7)
        var edges={}
        for i in range(0,indices.size(),3):
            for j in 3:
                var a=points[indices[i+j]]
                var b=points[indices[i+(j+1)%3]]
                if not edges.has(a):edges[a]=[]
                if not edges.has(b):edges[b]=[]
                edges[a].append(b)
                edges[b].append(a)
        var pending=[]
        for point in edges:
            if point.y<.2:pending.append(point)
        assert(not pending.is_empty())
        var visited={}
        var highest=0.0
        while not pending.is_empty():
            var point=pending.pop_back()
            if visited.has(point):continue
            visited[point]=true
            highest=max(highest,point.y)
            pending.append_array(edges[point])
        assert(highest>3.8)
    print("CONNECTED_TRUNK:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_trunk.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("CONNECTED_TRUNK:true", checked.stdout, checked.stderr)

    def test_covered_lods_preserve_shared_material_dependencies(self):
        material = self.recipe('return cooker.material({color={.2,.4,.3}})',
                               output="res://assets/runtime/material/shared/paint.res")
        source = self.recipe('''return cooker.scene({{
 asset=cooker.primitive("sphere",{radius=2,segments=24,rings=12}),
 material=cooker.input("paint")}})''', inputs={"paint":material["output"]},
                             output="res://assets/runtime/model/shared/source.scn")
        covered = self.recipe('''return cooker.minimum_visual("visual",{
 triangle_ratio=1,max_texture_size=32,lod_triangle_ratios={.8},
 lod_screen_errors={.3},lod_minimum_coverage=.95,preserve_materials=true})''',
                              inputs={"visual":source["output"]},
                              output="res://assets/runtime/model/shared/lods.scn")
        self.assertIsNotNone(REFERENCE, "Shared material readback requires the Veya editor")
        script = self.project / "inspect_lod_material.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var expected=load("'''+material["output"]+'''")
    var scene=load("'''+covered["output"]+'''").instantiate()
    var multi=scene.find_children("*","MultiMeshInstance3D",true,false)[0]
    assert(multi.material_override==expected)
    scene.free()
    print("SHARED_LOD_MATERIAL:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_lod_material.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("SHARED_LOD_MATERIAL:true", checked.stdout, checked.stderr)

    def test_covered_lods_reject_incomplete_or_invalid_policy(self):
        source = self.recipe(BOX, output="res://assets/runtime/model/policy/source.res")
        for policy in (
            "lod_screen_errors={.2},lod_minimum_coverage=.95",
            "lod_triangle_ratios={.5},lod_screen_errors={.2,.4},lod_minimum_coverage=.95",
            "lod_triangle_ratios={.5,.2},lod_screen_errors={.4,.2},lod_minimum_coverage=.95",
            "lod_triangle_ratios={.5},lod_screen_errors={.2}",
            "lod_triangle_ratios={.5},lod_minimum_coverage=.95",
            "lod_triangle_ratios={.5},lod_screen_errors={.2},lod_minimum_coverage=1.01",
            "lod_root_surfaces={1}",
            "lod_triangle_ratios={.5},lod_screen_errors={.2},lod_minimum_coverage=.9,lod_root_surfaces={0}",
            "lod_triangle_ratios={.5},lod_screen_errors={.2},lod_minimum_coverage=.9,lod_root_surfaces={1,1}",
            "lod_triangle_ratios={.5},lod_screen_errors={.2},lod_minimum_coverage=.9,lod_root_surfaces={2}",
        ):
            with self.subTest(policy=policy):
                self.recipe('return cooker.minimum_visual("mesh",{'+policy+'})',
                            success=False, inputs={"mesh":source["output"]},
                            output=f"res://assets/runtime/model/policy/invalid-{self.index}.res")

    def test_minimum_builds_far_lods_for_disconnected_geometry(self):
        source = self.recipe('''local vertices, indices = {}, {}
for y = 0, 3 do
    for z = 0, 7 do
        for x = 0, 7 do
            local first = #vertices
            local px, py, pz = x * 0.3, y * 0.4, z * 0.3
            for _, point in {
                { px, py, pz },
                { px + 0.12, py, pz },
                { px + 0.12, py + 0.16, pz + 0.03 },
                { px, py + 0.16, pz + 0.03 },
            } do
                table.insert(vertices, point)
            end
            for _, index in { 1, 2, 3, 1, 3, 4 } do
                table.insert(indices, first + index)
            end
        end
    end
end
return cooker.mesh({ vertices = vertices, indices = indices })''',
                             output="res://content/worlds/fixture/assets/runtime/model/lod/source.res")
        minimum = self.recipe('''return cooker.minimum_visual("mesh",{
 triangle_ratio=1,max_texture_size=32,
 lod_triangle_ratios={.5,.25,.125,.0625}})''',
                              inputs={"mesh": source["output"]},
                              output="res://content/worlds/fixture/assets/runtime/model/lod/minimum.res")
        self.assertIsNotNone(REFERENCE, "LOD resource round trip needs --reference-godot")
        script = self.project / "inspect_disconnected_lods.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var original = load("'''+source["output"]+'''")
    var minimum = load("'''+minimum["output"]+'''")
    var source_arrays = original.surface_get_arrays(0)
    var arrays = minimum.surface_get_arrays(0)
    var source_indices = source_arrays[Mesh.ARRAY_INDEX]
    var indices = arrays[Mesh.ARRAY_INDEX]
    assert(source_indices.size() == indices.size())
    for i in indices.size():
        assert(source_arrays[Mesh.ARRAY_VERTEX][source_indices[i]] == arrays[Mesh.ARRAY_VERTEX][indices[i]])
    var lods = ImporterMesh.from_mesh(minimum)
    var achieved = []
    for level in lods.get_surface_lod_count(0):
        achieved.append(lods.get_surface_lod_indices(0, level).size())
    print("DISCONNECTED_LOD_INDICES:", achieved)
    assert(ImporterMesh.from_mesh(original).get_surface_lod_count(0) == 0)
    # Tiny disconnected leaves may disappear at coarser targets. The contract
    # requires a useful valid level, not one level for every requested ratio.
    assert(lods.get_surface_lod_count(0) >= 1)
    var count = indices.size()
    var distance = 0.0
    for level in lods.get_surface_lod_count(0):
        var next = lods.get_surface_lod_indices(0, level)
        assert(next.size() >= 3 and next.size() < count and next.size() % 3 == 0)
        assert(lods.get_surface_lod_size(0, level) > distance)
        for index in next:
            assert(index >= 0 and index < arrays[Mesh.ARRAY_VERTEX].size())
        count = next.size()
        distance = lods.get_surface_lod_size(0, level)
    assert(count < indices.size() * 0.5)
    print("MINIMUM_DISCONNECTED_LODS:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_disconnected_lods.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("MINIMUM_DISCONNECTED_LODS:true", checked.stdout, checked.stderr)

    def test_minimum_retains_source_lods_after_union_compaction(self):
        source = self.recipe('return cooker.primitive("sphere",{radius=2,segments=48,rings=24})',
                             output="res://content/worlds/fixture/assets/runtime/model/lod/source.res")
        processed = self.job({"operation": "process-mesh", "source": source["output"],
                              "output": "res://content/worlds/fixture/assets/runtime/model/lod/source_lods.res"})
        minimum = self.recipe('''return cooker.minimum_visual("mesh",{
 triangle_ratio=.75,max_texture_size=32,lod_triangle_ratios={.03125,.015625}})''',
                              inputs={"mesh": processed["output"]},
                              output="res://content/worlds/fixture/assets/runtime/model/lod/minimum.res")
        self.assertIsNotNone(REFERENCE, "LOD resource round trip needs --reference-godot")
        script = self.project / "inspect_source_lods.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var source = load("'''+processed["output"]+'''")
    var minimum = load("'''+minimum["output"]+'''")
    var source_lods = ImporterMesh.from_mesh(source)
    var result_lods = ImporterMesh.from_mesh(minimum)
    var source_vertices = source.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
    var result_arrays = minimum.surface_get_arrays(0)
    var result_vertices = result_arrays[Mesh.ARRAY_VERTEX]
    var preserved = 0
    for source_level in source_lods.get_surface_lod_count(0):
        var expected = source_lods.get_surface_lod_indices(0, source_level)
        if expected.size() >= result_arrays[Mesh.ARRAY_INDEX].size():
            continue
        var found = false
        for result_level in result_lods.get_surface_lod_count(0):
            var actual = result_lods.get_surface_lod_indices(0, result_level)
            if expected.size() != actual.size():
                continue
            var matches = true
            for index in expected.size():
                matches = matches and source_vertices[expected[index]] == result_vertices[actual[index]]
            found = found or matches
        assert(found)
        preserved += 1
    assert(preserved > 0)
    print("MINIMUM_SOURCE_LODS:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_source_lods.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("MINIMUM_SOURCE_LODS:true", checked.stdout, checked.stderr)

    def test_minimum_far_lods_preserve_heightfield_protection(self):
        minimum = self.recipe('''local vertices, indices = {}, {}
for z = 0, 8 do
    for x = 0, 8 do
        table.insert(vertices, { x, 0, z })
    end
end
for z = 0, 7 do
    for x = 0, 7 do
        local first = z * 9 + x + 1
        for _, index in { first, first + 1, first + 9, first + 1, first + 10, first + 9 } do
            table.insert(indices, index)
        end
    end
end
return cooker.minimum_visual(cooker.mesh({ vertices = vertices, indices = indices }), {
    triangle_ratio = 1,
    preserve_heightfields = true,
    lod_triangle_ratios = { 0.5, 0.25, 0.125 },
})''', output="res://content/worlds/fixture/assets/runtime/model/lod/heightfield.res")
        self.assertIsNotNone(REFERENCE, "LOD resource round trip needs --reference-godot")
        script = self.project / "inspect_protected_lods.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var mesh = load("'''+minimum["output"]+'''")
    assert(ImporterMesh.from_mesh(mesh).get_surface_lod_count(0) == 0)
    assert(mesh.surface_get_arrays(0)[Mesh.ARRAY_INDEX].size() == 384)
    print("MINIMUM_PROTECTED_LODS:true")
    quit()
''', encoding="utf-8")
        checked = subprocess.run(
            [str(REFERENCE), "--headless", "--path", str(self.project),
             "--script", "res://inspect_protected_lods.gd", "--quit-after", "20"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("MINIMUM_PROTECTED_LODS:true", checked.stdout, checked.stderr)

    def test_minimum_rejects_invalid_far_lod_ratios(self):
        for value in ("{}", "{1}", "{0}", "{-.1}", "{.5,.5}", "{.25,.5}",
                      "{.9,.8,.7,.6,.5,.4,.3,.2,.1}"):
            with self.subTest(value=value):
                self.recipe('''return cooker.minimum_visual(cooker.primitive("sphere",{}),{
 lod_triangle_ratios='''+value+'''})''', success=False,
                            output="res://content/worlds/fixture/assets/runtime/model/lod/refused.res")

    def test_minimum_color_texture_compression_survives_resource_save(self):
        source = self.recipe('''local texture=cooker.procedural_texture({kind="soft-noise",size=64,seed=17})
local material=cooker.material({albedo_texture=texture})
return cooker.scene({{asset=cooker.primitive("box",{}),material=material}})''',
                             output="res://content/worlds/fixture/assets/runtime/material/minimum-test/original.scn")
        minimum = self.recipe('''return cooker.minimum_visual("visual",{
 triangle_ratio=1,max_texture_size=32,compress_color_textures=true})''',
                              inputs={"visual": source["output"]},
                              output="res://content/worlds/fixture/assets/runtime/material/minimum-test/minimum.scn")
        report = self.job({"operation": "validate-resource", "source": minimum["output"],
                           "type": "PackedScene"})
        self.assertEqual(report["node_count"], 2)
        self.assertIsNotNone(REFERENCE, "compression round trip needs --reference-godot")
        with (self.project / "project.godot").open("a", encoding="utf-8") as project:
            project.write("config/disable_project_settings_override=false\n")
        script = self.project / "inspect_minimum.gd"
        script.write_text('''extends SceneTree
func _initialize():
    var scene = load("'''+minimum["output"]+'''").instantiate()
    var geometry = scene.get_child(0) as GeometryInstance3D
    var mesh = geometry.mesh if geometry is MeshInstance3D else geometry.multimesh.mesh
    var material = geometry.material_override as BaseMaterial3D
    if material == null:
        material = mesh.surface_get_material(0) as BaseMaterial3D
    var texture = material.get_texture(BaseMaterial3D.TEXTURE_ALBEDO)
    var valid = texture is PortableCompressedTexture2D and texture.get_width() == 32
    valid = valid and texture.get_compression_mode() == PortableCompressedTexture2D.COMPRESSION_MODE_BASIS_UNIVERSAL
    scene.free()
    print("MINIMUM_TEXTURE_ROUND_TRIP:", valid)
    quit(0 if valid else 1)
''', encoding="utf-8")
        checked = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                                  "--script", "res://inspect_minimum.gd", "--quit-after", "20"],
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout+checked.stderr)
        self.assertIn("MINIMUM_TEXTURE_ROUND_TRIP:true", checked.stdout,
                      checked.stdout+checked.stderr)

    def test_minimum_channel_compression_preserves_float_data(self):
        directory = self.project / "content/worlds/fixture/assets/runtime/material/minimum-test"
        directory.mkdir(parents=True)
        textures = {
            "mask": ("RedGreen", bytes([64, 192]) * (64 * 64)),
            "normal_map": ("RedGreen", bytes([128, 128]) * (64 * 64)),
            "roughness": ("Red8", bytes([96]) * (64 * 64)),
            "field": ("RGFloat", struct.pack("<ff", 0.125, 3.5) * (64 * 64)),
        }
        for name, (image_format, payload) in textures.items():
            (directory / f"{name}.tres").write_text(
                '[gd_resource type="ImageTexture" load_steps=2 format=3]\n'
                '[sub_resource type="Image" id="Image"]\n'
                'data = {"width":64,"height":64,"mipmaps":false,'
                f'"format":"{image_format}","data":PackedByteArray('
                + ','.join(map(str, payload)) + ')}\n'
                '[resource]\nimage=SubResource("Image")\n')
        (directory / "ground.tres").write_text(
            '[gd_resource type="ShaderMaterial" load_steps=6 format=3]\n'
            + ''.join(f'[ext_resource type="Texture2D" path="{name}.tres" id="{name}"]\n'
                      for name in textures)
            + '[sub_resource type="Shader" id="Shader"]\n'
              'code="shader_type spatial; uniform sampler2D mask; '
              'uniform sampler2D normal_map : hint_normal; '
              'uniform sampler2D roughness; uniform sampler2D field;"\n'
              '[resource]\nshader=SubResource("Shader")\n'
            + ''.join(f'shader_parameter/{name}=ExtResource("{name}")\n' for name in textures))
        minimum = self.recipe('''return cooker.minimum_visual("material",{
 max_texture_size=32,compress_color_textures=true})''',
                              inputs={"material": "res://content/worlds/fixture/assets/runtime/material/minimum-test/ground.tres"},
                              output="res://content/worlds/fixture/assets/runtime/material/minimum-test/ground-minimum.res")
        self.assertIsNotNone(REFERENCE)
        with (self.project / "project.godot").open("a") as project:
            project.write("config/disable_project_settings_override=false\n")
        (self.project / "inspect_channels.gd").write_text('''extends SceneTree
func _initialize():
    var material = load("'''+minimum["output"]+'''") as ShaderMaterial
    var valid = material != null
    for name in ["mask", "normal_map", "roughness"]:
        var texture = material.get_shader_parameter(name)
        valid = valid and texture is PortableCompressedTexture2D
        valid = valid and texture.get_width() == 32 and texture.get_height() == 32
        if texture is PortableCompressedTexture2D:
            valid = valid and texture.get_compression_mode() == PortableCompressedTexture2D.COMPRESSION_MODE_BASIS_UNIVERSAL
    var source = load("res://content/worlds/fixture/assets/runtime/material/minimum-test/field.tres").get_image()
    var field = material.get_shader_parameter("field").get_image()
    valid = valid and field.get_format() == source.get_format()
    valid = valid and field.get_size() == source.get_size()
    valid = valid and field.get_data() == source.get_data()
    print("MINIMUM_CHANNELS_AND_FIELD:", valid)
    quit(0 if valid else 1)
''')
        inspected = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                                    "--script", "res://inspect_channels.gd"],
                                   capture_output=True, text=True, timeout=30)
        self.assertEqual(inspected.returncode, 0, inspected.stdout+inspected.stderr)
        self.assertIn("MINIMUM_CHANNELS_AND_FIELD:true", inspected.stdout)

    def test_scenes_reuse_saved_minimum_material(self):
        original = self.recipe('return cooker.material({roughness=0.37})',
                               output="res://content/worlds/fixture/assets/runtime/material/minimum-test/original.res")
        material = self.recipe('return cooker.minimum_visual("material",{max_texture_size=32})',
                               inputs={"material": original["output"]},
                               output="res://content/worlds/fixture/assets/runtime/material/minimum-test/ground-minimum.res")
        scenes = []
        for index in range(2):
            result = self.recipe('''return cooker.scene({{
 asset=cooker.primitive("box",{}),material=cooker.input("material")}})''',
                                 inputs={"material": material["output"]},
                                 output=f"res://content/worlds/fixture/assets/runtime/material/minimum-test/chunk-{index}.scn")
            scenes.append(result["output"])
        self.assertIsNotNone(REFERENCE)
        with (self.project / "project.godot").open("a") as project:
            project.write("config/disable_project_settings_override=false\n")
        (self.project / "inspect_shared_material.gd").write_text('''extends SceneTree
func _initialize():
    var first = load("'''+scenes[0]+'''").instantiate()
    var second = load("'''+scenes[1]+'''").instantiate()
    var material = load("'''+material["output"]+'''")
    var valid = first.get_child(0).material_override == material
    valid = valid and second.get_child(0).material_override == material
    first.free()
    second.free()
    print("MINIMUM_SHARED_MATERIAL:", valid)
    quit(0 if valid else 1)
''')
        inspected = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                                    "--script", "res://inspect_shared_material.gd"],
                                   capture_output=True, text=True, timeout=30)
        self.assertEqual(inspected.returncode, 0, inspected.stdout+inspected.stderr)
        self.assertIn("MINIMUM_SHARED_MATERIAL:true", inspected.stdout)

    def test_minimum_materials_share_external_shader(self):
        (self.project / "shared.gdshader").write_text(
            "shader_type spatial; void fragment(){ALBEDO=vec3(0.5);}")
        for name, priority in (("a", 1), ("b", 2)):
            (self.project / f"{name}.tres").write_text(
                '[gd_resource type="ShaderMaterial" load_steps=2 format=3]\n'
                '[ext_resource type="Shader" path="res://shared.gdshader" id="1"]\n'
                f'[resource]\nshader=ExtResource("1")\nrender_priority={priority}\n')
            self.job({"operation": "save-resource", "source": f"res://{name}.tres",
                      "output": f"res://content/worlds/fixture/generated/{name}.res",
                      "type": "ShaderMaterial"})
        original = self.recipe('''local mesh=cooker.primitive("box",{})
return cooker.scene({{asset=mesh,material=cooker.input("a")},
 {asset=mesh,material=cooker.input("b"),position={3,0,0}}})''',
                               inputs={"a": "res://content/worlds/fixture/generated/a.res",
                                       "b": "res://content/worlds/fixture/generated/b.res"},
                               output="res://content/worlds/fixture/generated/source.scn")
        minimum = self.recipe('return cooker.minimum_visual("visual",{triangle_ratio=1})',
                              inputs={"visual": original["output"]},
                              external_render_resources=True,
                              output="res://content/worlds/fixture/generated/minimum/model.scn")
        self.assertIsNotNone(REFERENCE)
        with (self.project / "project.godot").open("a") as project:
            project.write("config/disable_project_settings_override=false\n")
        (self.project / "inspect_shader.gd").write_text('''extends SceneTree
func _initialize():
    var scene = load("'''+minimum["output"]+'''").instantiate()
    var first = scene.get_child(0).material_override as ShaderMaterial
    var second = scene.get_child(1).material_override as ShaderMaterial
    var valid = first != null and second != null and first != second
    valid = valid and first.shader == second.shader and not first.shader.resource_path.is_empty()
    valid = valid and first.render_priority == 1 and second.render_priority == 2
    scene.free()
    print("MINIMUM_SHARED_SHADER:", valid)
    quit(0 if valid else 1)
''')
        inspected = subprocess.run([str(REFERENCE), "--headless", "--path", str(self.project),
                                    "--script", "res://inspect_shader.gd"],
                                   capture_output=True, text=True, timeout=30)
        self.assertEqual(inspected.returncode, 0, inspected.stdout+inspected.stderr)
        self.assertIn("MINIMUM_SHARED_SHADER:true", inspected.stdout)

    def test_shared_render_cache_checks_auxiliary_files(self):
        original = self.recipe('local mesh=cooker.primitive("box",{}); return cooker.scene({{asset=mesh}})',
                               output="res://content/worlds/fixture/generated/original.scn")
        (self.project / "fragment.luau").write_text('return cooker.scene({{asset=cooker.input("visual"),keep={"Batch0"}}})')
        code = 'return {{operation="run-recipe",source="res://fragment.luau",inputs={visual="'+original["output"]+'"},output="res://content/worlds/fixture/generated/parts/part.scn",external_render_resources=true}}'
        first = self.pipeline(code)["steps"][0]
        shared = self.project / first["render_outputs"][0].removeprefix("res://")
        expected = shared.read_bytes()
        self.assertTrue(self.pipeline(code)["steps"][0]["skipped"])
        shared.unlink()  # This isolated fixture owns the resource.
        restored = self.pipeline(code)["steps"][0]
        self.assertFalse(restored.get("skipped", False))
        self.assertEqual(shared.read_bytes(), expected)
        replacement = self.recipe(
            'local mesh=cooker.primitive("sphere",{}); return cooker.scene({{asset=mesh}})',
            output="res://content/worlds/fixture/generated/replacement.scn")
        shared.unlink()
        code = code.replace(original["output"], replacement["output"])
        rebuilt = self.pipeline(code)["steps"][0]
        self.assertFalse(rebuilt.get("skipped", False))
        receipt = json.loads((self.project / (rebuilt["output"] + ".cook.json").removeprefix("res://")).read_text())
        self.assertEqual(set(receipt["auxiliary_outputs"]), set(rebuilt["render_outputs"]))
        self.assertNotIn("res://" + str(shared.relative_to(self.project)).replace("\\", "/"),
                         receipt["auxiliary_outputs"])
        shared = self.project / rebuilt["render_outputs"][0].removeprefix("res://")
        shared.write_bytes(b"changed dependency")
        self.pipeline(code, success=False)

    def test_large_bounded_fragment_manifest(self):
        source = self.recipe(output="res://content/worlds/fixture/generated/model/fragments/part_0.res")
        directory = self.project / "content/worlds/fixture/generated/model/fragments"
        payload = (self.project / source["output"].removeprefix("res://")).read_bytes()
        for index in range(1, 300):
            (directory / f"part_{index}.res").write_bytes(payload)
        code = '''local files={}
for index=0,299 do table.insert(files,"part_"..index..".res") end
return {{operation="asset-manifest",output="res://content/worlds/fixture/generated/model/fragments/asset.manifest.json",
 asset_id="fragments",revision=1,kind="model",entry=files[1],files=files,source="fixture",generator="test",if_missing=true}}
'''
        first = self.pipeline(code)
        self.assertEqual(len(first["steps"][0]["files"]), 300)
        self.assertTrue(self.pipeline(code)["steps"][0]["skipped"])
        self.pipeline(code.replace("0,299", "0,2048"), success=False)

    def test_input_shader_include_provenance(self):
        directory = self.project / "content/worlds/fixture/generated"
        directory.mkdir(parents=True)
        (directory / "color.gdshaderinc").write_text('const vec3 RECIPE_TINT = vec3(0.3,0.4,0.5);\n')
        (directory / "surface.gdshader").write_text('shader_type spatial;\n#include "res://content/worlds/fixture/generated/color.gdshaderinc"\nvoid fragment() { ALBEDO = RECIPE_TINT; }\n')
        (directory / "finish.tres").write_text('[gd_resource type="ShaderMaterial" load_steps=2 format=3]\n[ext_resource type="Shader" path="res://content/worlds/fixture/generated/surface.gdshader" id="1"]\n[resource]\nshader=ExtResource("1")\n')
        result = self.recipe('return cooker.input("finish")', inputs={"finish": "res://content/worlds/fixture/generated/finish.tres"})
        self.assertEqual(set(result["input_sha256"]), {"res://content/worlds/fixture/generated/" + name for name in
                                                     ("finish.tres", "surface.gdshader", "color.gdshaderinc")})

    def test_material_edit_rebuilds_only_its_own_step(self):
        # A scene that references a material keeps its cache when only the
        # material's parameters change; the material step itself rebuilds.
        directory = self.project / "content/worlds/fixture/source"
        directory.mkdir(parents=True)
        material = directory / "finish.tres"
        material.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\nroughness=0.25\n')
        (self.project / "scene.luau").write_text('return cooker.scene({{asset=cooker.primitive("box",{}),material=cooker.input("finish")}})')
        code = '''local g="res://content/worlds/fixture/generated/"
return {
 {name="material",operation="save-resource",source="res://content/worlds/fixture/source/finish.tres",output=g.."finish.res",type="StandardMaterial3D"},
 {name="scene",operation="run-recipe",source="res://scene.luau",output=g.."scene.scn",inputs={finish=g.."finish.res"}},
 {name="layout",operation="run-recipe",source="res://layout.luau",output=g.."layout.scn",inputs={scene=g.."scene.scn"}},
}'''
        (self.project / "layout.luau").write_text('return cooker.scene({{asset=cooker.input("scene")}})')
        first = {step["name"]: step for step in self.pipeline(code)["steps"]}
        self.assertFalse(any(step.get("skipped") for step in first.values()), first)
        material.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\nroughness=0.75\n')
        second = {step["name"]: step for step in self.pipeline(code)["steps"]}
        self.assertFalse(second["material"].get("skipped"), second)
        # The scene declares the material directly, so it rebuilds by content.
        self.assertFalse(second["scene"].get("skipped"), second)
        # The layout only reaches the material through the scene's path reference.
        self.assertTrue(second["layout"].get("skipped"), second)

    def test_rejects_source_errors_and_invalid_returns(self):
        for code in ('return ???', 'error("intentional")', 'return nil', 'return {}', 'return 1',
                     'return cooker.primitive("box",{}), 1', b'\x00\x04bytecode', b'\xff\xfe',
                     '--' + 'a' * 65536):
            with self.subTest(code=str(code)[:35]):
                self.recipe(code, success=False)
        self.recipe(output="res://content/worlds/fixture/generated/wrong.scn", success=False)

    def test_blocked_libraries_and_readonly_environment(self):
        self.recipe('''for _, name in {"os", "io", "debug", "require", "loadstring", "dofile", "loadfile", "package", "coroutine", "getfenv", "setfenv", "newproxy", "print"} do
    assert(_G[name] == nil, name)
end
assert(not pcall(function() return os.clock() end))
assert(not pcall(function() return math.random() end))
assert(not pcall(function() return vector.create(1,2,3) end))
assert(not pcall(function() cooker.parameters.nested[1] = 2 end))
assert(not pcall(function() cooker.mesh = nil end))
assert(not pcall(function() math.sin = nil end))
''' + BOX, parameters={"nested": [1]})

    def test_timeouts_cannot_be_swallowed(self):
        for code in ('while true do end',
                     'pcall(function() while true do end end); ' + BOX,
                     'while true do xpcall(function() while true do end end, function() return 1 end) end'):
            with self.subTest(code=code[:30]):
                report = self.recipe(code, limits={"time_ms": 10}, success=False)
                self.assertIn("budget", report["message"])

    def test_recipes_have_no_default_deadline(self):
        # Several seconds of pure Luau: beyond the former 2 s default deadline.
        report = self.recipe('local n=0 for i=1,80000000 do n=n+i%7 end assert(n>0) ' + BOX)
        self.assertEqual(report["limits"]["time_ms"], 0)

    def test_vm_memory_failure_is_sticky(self):
        report = self.recipe('pcall(function() return string.rep("x", 8*1024*1024) end); ' + BOX,
                             limits={"memory_mb": 1}, success=False)
        self.assertIn("memory", report["message"])

    def test_host_errors_cannot_be_swallowed(self):
        for code in ('cooker.primitive("box", {size={-1,2,3}})',
                     'cooker.input("undeclared")', 'cooker.mesh({vertices={},indices={}})',
                     'cooker.primitive("box", {siz=3})'):
            with self.subTest(code=code):
                self.recipe('pcall(function() ' + code + ' end); ' + BOX, success=False)

    def test_removed_capacity_options_and_unlimited_instances(self):
        self.recipe('cooker.material({}); ' + BOX, limits={"max_resources": 1}, success=False)
        self.recipe(limits={"max_vertices": 3}, success=False)
        self.recipe('local b=cooker.primitive("box",{}); return cooker.scene({{asset=b},{asset=b}})',
                    limits={"max_instances": 1}, success=False, output="res://content/worlds/fixture/generated/too-many.scn")
        # No default instance ceiling: dense scatter cells may exceed the former 10000 cap.
        self.recipe('local b=cooker.primitive("box",{}); local p={}; for i=1,12000 do p[i]={asset=b,position={i,0,0}} end; return cooker.scene(p)',
                    output="res://content/worlds/fixture/generated/many.scn")

    def test_invalid_configuration_and_mesh_data(self):
        expressions = [
            'cooker.primitive("unknown",{})', 'cooker.primitive("box",{size={"1",2,3}})',
            'cooker.primitive("box",{size={1,math.huge,3}})', 'cooker.primitive("box",{size={1,0/0,3}})',
            'cooker.primitive("sphere",{segments=4.5})', 'cooker.primitive("cylinder",{segments=100000})',
            'cooker.primitive("box",{["size\\0bad"]={1,2,3}})',
            'cooker.material({metallic=true})', 'cooker.material({color={1,2,3}})',
            'cooker.mesh({vertices={{0,0,0},{1,0,0},{0,1,0}},indices={1,2,4}})',
            'cooker.mesh({vertices={{0,0,0},{1,0,0},{0,1,0}},indices={1,1,1}})',
            'cooker.mesh({vertices={{0,0,0},{1,0,0},{0,1,0}},indices={1,2,3.5}})',
            'cooker.mesh({vertices={{0,0,0},{1,0,0},{0,1,0}},indices={1,2}})',
            'cooker.mesh({vertices={{0,0,0},{1,0,0},{0,1,0}},indices={1,2,3},normals={{0,0,1}}})',
            'cooker.scene({{asset=12}})',
        ]
        for expression in expressions:
            with self.subTest(expression=expression): self.recipe('return ' + expression, success=False)

    def test_raw_fields_do_not_invoke_metamethods(self):
        self.recipe('return cooker.primitive("box",setmetatable({}, {__index=function() error("must not run") end}))')

    def test_job_paths_inputs_and_limits_are_validated(self):
        for fields in ({"seed": -1}, {"seed": True}, {"seed": 1.5}, {"seed": 2**32},
                       {"limits": {"time_ms": 0}}, {"limits": {"memory_mb": 0}}, {"limits": {"memory_mb": 33}},
                       {"limits": {"extra": 2}}, {"parameters": []}, {"parameters": {"v": None}},
                       {"inputs": {"bad": "res://recipe-0.luau"}}, {"inputs": {"bad": "user://secret.res"}},
                       {"unknown": 1}, {"output": "res://content/worlds/fixture/generated/../escape.res"},
                       {"output": "user://escape.res"}, {"output": "res://assets/escape.res"}):
            with self.subTest(fields=fields): self.recipe(success=False, **fields)
        deep = {}
        for _ in range(18): deep = {"nested": deep}
        self.recipe(parameters=deep, success=False)

    def test_existing_output_is_not_modified(self):
        output = self.recipe()["output"]
        target = self.project / output.removeprefix("res://")
        before = target.read_bytes()
        source = self.project / "replacement.luau"
        source.write_text('return cooker.material({})')
        self.job({"operation": "run-recipe", "source": "res://replacement.luau", "output": output}, False)
        self.assertEqual(target.read_bytes(), before)

    def test_large_output_keeps_its_input_unchanged(self):
        directory = self.project / "content/worlds/fixture/generated"
        directory.mkdir(parents=True)
        name = base64.b64encode(random.Random(42).randbytes(1400000)).decode()
        asset = directory / "large.tres"
        asset.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\nresource_name="' + name + '"\n')
        digest = hashlib.sha256(asset.read_bytes()).hexdigest()
        self.recipe('return cooker.input("large")', inputs={"large": "res://content/worlds/fixture/generated/large.tres"},
                    success=True)
        self.assertEqual(hashlib.sha256(asset.read_bytes()).hexdigest(), digest)

    def test_symlink_input_escape_is_rejected(self):
        directory = self.project / "content/worlds/fixture/generated"
        directory.mkdir(parents=True)
        with tempfile.TemporaryDirectory(prefix="veya-recipe-outside-") as outside:
            asset = Path(outside) / "outside.tres"
            asset.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\n')
            try:
                (directory / "link.tres").symlink_to(asset)
            except OSError:
                self.skipTest("creating symlinks requires platform permission")
            self.recipe('return cooker.input("escape")', inputs={"escape": "res://content/worlds/fixture/generated/link.tres"}, success=False)

    def test_invalid_instance_transforms_and_dense_arrays(self):
        for item in ('{asset=b,scale={0,1,1}}', '{asset=b,basis={{1,0,0},{0,1,0},{0,0,1}},scale={1,1,1}}',
                     '{asset=b,basis={{1,0,0},{2,0,0},{0,0,1}}}', '{asset=b,position={1,2}}',
                     '{asset=b,position={[1]=1,[3]=3}}', '{asset=b,position={1,2,3,named=1}}'):
            with self.subTest(item=item):
                self.recipe('local b=cooker.primitive("box",{}); return cooker.scene({' + item + '})',
                            output=f"res://content/worlds/fixture/generated/bad-transform-{self.index}.scn", success=False)

    @unittest.skipUnless(REFERENCE, "pass --reference-godot for independent packed-resource verification")
    def test_pack_loads_in_empty_godot_project_with_full_transforms(self):
        self.recipe('''local b=cooker.primitive("box",{size={2,3,4}})
local m=cooker.material({color={0.2,0.4,0.6},roughness=0.37,metallic=0.72})
return cooker.scene({
 {asset=b,material=m,position={2,3,4},basis={{0,2,0},{-3,0,0},{0,0,4}}},
 {asset=b,material=m,position={5,6,7},scale={2,3,4}},
 {asset=b,position={0,0,0}}
})''', output="res://content/worlds/fixture/generated/checked.scn")
        pack = "content/releases/checked.pck"
        self.job({"operation": "pack", "files": ["res://content/worlds/fixture/generated/checked.scn"], "output": "res://" + pack})
        verify = self.project / "isolated"
        verify.mkdir()
        (verify / "project.godot").write_text('config_version=5\n')
        (verify / "verify.gd").write_text('''extends SceneTree
var failed := false
func check(ok: bool, message: String) -> void:
    if not ok:
        push_error(message)
        failed = true
func transform_from_buffer(buffer: PackedFloat32Array, index: int) -> Transform3D:
    # Official headless Dummy get_instance_transform() returns identity.
    # Its serialized buffer is preserved; decode all columns instead.
    var offset := index * 12
    return Transform3D(Basis(Vector3(buffer[offset],buffer[offset+4],buffer[offset+8]),
        Vector3(buffer[offset+1],buffer[offset+5],buffer[offset+9]),
        Vector3(buffer[offset+2],buffer[offset+6],buffer[offset+10])),
        Vector3(buffer[offset+3],buffer[offset+7],buffer[offset+11]))
func _initialize() -> void:
    check(ProjectSettings.load_resource_pack(OS.get_cmdline_user_args()[0]), "mount failed")
    var scene = load("res://content/worlds/fixture/generated/checked.scn").instantiate()
    check(scene.get_child_count() == 2, "batch count")
    var batch = scene.get_node("Batch0")
    check(batch.multimesh.instance_count == 2, "instance count")
    var first: Transform3D = transform_from_buffer(batch.multimesh.buffer, 0)
    check(first.origin.is_equal_approx(Vector3(2,3,4)), "position")
    check(first.basis.x.is_equal_approx(Vector3(0,2,0)), "basis x")
    check(first.basis.y.is_equal_approx(Vector3(-3,0,0)), "basis y")
    check(first.basis.z.is_equal_approx(Vector3(0,0,4)), "basis z")
    var second: Transform3D = transform_from_buffer(batch.multimesh.buffer, 1)
    check(second.basis.get_scale().is_equal_approx(Vector3(2,3,4)), "scale")
    check(is_equal_approx(batch.material_override.roughness,0.37), "roughness")
    check(is_equal_approx(batch.material_override.metallic,0.72), "metallic")
    check(batch.multimesh.mesh.size.is_equal_approx(Vector3(2,3,4)), "geometry")
    scene.free()
    if not failed: print("COOKER_RECIPE_PACK_OK")
    quit(2 if failed else 0)
''')
        run = subprocess.run([str(REFERENCE), "--headless", "--path", str(verify), "--script", str(verify / "verify.gd"),
                              "--", str(self.project / pack)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("COOKER_RECIPE_PACK_OK", run.stdout)
        self.assertNotIn("ERROR:", run.stdout + run.stderr)

    def test_nested_resource_bytes_repeat_with_identical_output_path(self):
        source = self.project / "deterministic.luau"
        source.write_text('local mesh=cooker.primitive("box",{}); return cooker.scene({{asset=mesh}})')
        output = "res://content/worlds/fixture/generated/deterministic.scn"
        job = {"operation": "run-recipe", "source": "res://deterministic.luau", "output": output, "seed": 73}
        target = self.project / "content/worlds/fixture/generated/deterministic.scn"
        self.job(job)
        first = target.read_bytes()
        target.unlink()  # Isolated test fixture only; the next run uses the same output path.
        self.job(job)
        self.assertEqual(first, target.read_bytes())


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining])
# /*>>----- VEYA_COOKER */
