# /*<<----- VEYA_COOKER: native recipe integration and fault tests; Python is only the test harness. */
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
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
                             capture_output=True, text=True, timeout=20, env=environment)
        self.assertGreaterEqual(run.returncode, 0, run.stdout + run.stderr)  # No signal/crash on bad input.
        lines = [line for line in run.stdout.splitlines() if line.startswith('{')]
        self.assertTrue(lines, run.stdout + run.stderr)
        result = json.loads(lines[-1])
        self.assertEqual(result["ok"], success, result)
        self.assertEqual(run.returncode == 0, success, run.stdout + run.stderr)
        if success:
            self.assertNotIn("ERROR:", run.stdout + run.stderr)
        self.assertNotIn("leaked", run.stderr.lower(), run.stderr)
        return result

    def recipe(self, code=BOX, success=True, **fields):
        source = self.project / f"recipe-{self.index}.luau"
        source.write_bytes(code if isinstance(code, bytes) else code.encode())
        output = fields.pop("output", f"res://assets/generated/result-{self.index}.res")
        result = self.job({"operation": "run-recipe", "source": "res://" + source.name,
                           "output": output, **fields}, success)
        target = self.project / output.removeprefix("res://")
        if success:
            self.assertTrue(target.is_file())
            self.assertEqual(result["source_sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(result["sha256"], hashlib.sha256(target.read_bytes()).hexdigest())
        elif output.startswith("res://assets/generated/"):
            self.assertFalse(target.exists(), result)
        self.assertFalse(list(self.project.rglob("*.recipe-*")))
        return result

    def pipeline(self, code, success=True):
        source = self.project / f"pipeline-{self.index}.luau"
        source.write_bytes(code if isinstance(code, bytes) else code.encode())
        environment = os.environ.copy()
        environment["PATH"] = str(self.project / "no-external-executables")
        run = subprocess.run([str(EXECUTABLE), "--path", str(self.project), "--pipeline", "res://" + source.name],
                             capture_output=True, text=True, timeout=20, env=environment)
        lines = [line for line in run.stdout.splitlines() if line.startswith('{')]
        self.assertTrue(lines, run.stdout + run.stderr)
        result = json.loads(lines[-1])
        self.assertEqual(result["ok"], success, result)
        self.assertEqual(run.returncode == 0, success, run.stdout + run.stderr)
        return result

    def test_capabilities_and_no_external_runtime(self):
        run = subprocess.run([str(EXECUTABLE), "--capabilities"], capture_output=True, text=True, timeout=15)
        report = json.loads(next(line for line in run.stdout.splitlines() if line.startswith('{')))
        self.assertEqual(report["script_languages"], 0)
        self.assertEqual(report["recipe_runtime"]["version"], "0.738")
        self.assertEqual(report["recipe_runtime"]["api_version"], 2)
        self.assertIn("scene", report["recipe_runtime"]["apis"])
        self.assertEqual(report["pipeline_runtime"]["language"], "Luau")
        self.assertEqual(report["pipeline_runtime"]["api_version"], 1)
        # All recipe jobs below run with PATH pointing at an empty/nonexistent directory.
        report = self.recipe()
        self.assertEqual(report["type"], "BoxMesh")
        self.assertGreater(report["vertices"], 0)

    def test_luau_pipeline_owns_generation_validation_manifest_and_pack(self):
        (self.project / "asset.luau").write_text(BOX)
        code = '''local output = "res://assets/generated/example/r1/example.res"
return {
 {name="generate",operation="run-recipe",source="res://asset.luau",output=output,if_missing=true},
 {name="validate",operation="validate-resource",source=output,type="BoxMesh"},
 {name="manifest",operation="asset-manifest",output="res://assets/generated/example/r1/asset.manifest.json",
  asset_id="example",revision=1,kind="model",entry="example.res",files={"example.res"},
  source="test-source",generator="test-generator",if_missing=true},
 {name="pack",operation="pack",files={output},output="res://content/releases/example.pck",if_missing=true},
}'''
        result = self.pipeline(code)
        self.assertEqual(result["step_count"], 4)
        self.assertTrue(all(step["ok"] for step in result["steps"]))
        manifest = json.loads((self.project / "assets/generated/example/r1/asset.manifest.json").read_text())
        self.assertEqual(manifest["pipeline"], "veya-asset-cooker-luau")
        self.assertEqual(manifest["entry"], "example.res")
        self.assertEqual(len(manifest["files"][0]["sha256"]), 64)
        repeated = self.pipeline(code)
        self.assertTrue(repeated["steps"][0]["skipped"])
        self.assertFalse(repeated["steps"][1].get("skipped", False))
        self.assertTrue(repeated["steps"][2]["skipped"])
        self.assertTrue(repeated["steps"][3]["skipped"])

    def test_luau_pipeline_rejects_non_declarative_or_unsafe_values(self):
        cached = self.project / "assets/generated/cached.res"
        cached.parent.mkdir(parents=True)
        cached.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\n')
        for code in ("return {}", "return {{operation='validate-resource', source=os.getenv('X')}}",
                     "return {{operation='validate-resource', source=function() end}}",
                     "return {{[1]='mixed',operation='validate-resource'}}",
                     "return {{operation='unknown',output='res://assets/generated/cached.res',if_missing=true}}",
                     "return {{operation='validate-resource',source='res://assets/generated/cached.res',if_missing=true}}",
                     "while true do end"):
            with self.subTest(code=code[:40]):
                self.pipeline(code, success=False)

    def test_primitives_material_and_mesh(self):
        for code, kind in [
            ('return cooker.primitive("sphere", {radius=2,segments=12,rings=6})', "SphereMesh"),
            ('return cooker.primitive("cylinder", {height=3,radius=1,top_radius=0})', "CylinderMesh"),
            ('return cooker.material({color={0.3,0.4,0.5},metallic=0.8,roughness=0.4})', "StandardMaterial3D"),
            ('return cooker.mesh({vertices={{0,0,0},{1,0,0},{0,0,1}},indices={1,2,3},uvs={{0,0},{1,0},{0,1}}})', "ArrayMesh"),
        ]:
            with self.subTest(kind=kind):
                result = self.recipe(code)
                self.assertEqual(result["type"], kind)
                self.job({"operation": "validate-resource", "source": result["output"], "type": kind})

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
            result = self.recipe(code, output=f"res://assets/generated/bridge-{segments}.scn",
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
            output="res://assets/generated/from-input.scn")
        self.assertEqual(set(result["input_sha256"]), {mesh["output"], material["output"]})
        pack = self.job({"operation": "pack", "files": [result["output"]], "output": "res://content/releases/inputs.pck"})
        self.assertEqual(set(pack["files"]), {mesh["output"], material["output"], result["output"]})

    def test_packed_scene_bounds_removal_and_luau_composition(self):
        base = self.recipe('local b=cooker.primitive("box",{size={8,1,8}}); return cooker.scene({{asset=b}})',
                           output="res://assets/generated/base.scn")
        model = self.recipe('local b=cooker.primitive("box",{size={2,4,2}}); return cooker.scene({{asset=b}})',
                            output="res://assets/generated/model.scn")
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
            output="res://assets/generated/composed.scn")
        self.assertEqual(result["type"], "PackedScene")
        report = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(report["node_count"], 4)
        self.recipe('return cooker.scene({{asset=cooker.input("base"),remove={"Missing"}}})',
                    inputs={"base": base["output"]}, output="res://assets/generated/bad-removal.scn", success=False)

    def test_input_shader_include_provenance(self):
        directory = self.project / "assets/generated"
        directory.mkdir(parents=True)
        (directory / "color.gdshaderinc").write_text('const vec3 RECIPE_TINT = vec3(0.3,0.4,0.5);\n')
        (directory / "surface.gdshader").write_text('shader_type spatial;\n#include "res://assets/generated/color.gdshaderinc"\nvoid fragment() { ALBEDO = RECIPE_TINT; }\n')
        (directory / "finish.tres").write_text('[gd_resource type="ShaderMaterial" load_steps=2 format=3]\n[ext_resource type="Shader" path="res://assets/generated/surface.gdshader" id="1"]\n[resource]\nshader=ExtResource("1")\n')
        result = self.recipe('return cooker.input("finish")', inputs={"finish": "res://assets/generated/finish.tres"})
        self.assertEqual(set(result["input_sha256"]), {"res://assets/generated/" + name for name in
                                                     ("finish.tres", "surface.gdshader", "color.gdshaderinc")})

    def test_rejects_source_errors_and_invalid_returns(self):
        for code in ('return ???', 'error("intentional")', 'return nil', 'return {}', 'return 1',
                     'return cooker.primitive("box",{}), 1', b'\x00\x04bytecode', b'\xff\xfe',
                     '--' + 'a' * 65536):
            with self.subTest(code=str(code)[:35]):
                self.recipe(code, success=False)
        self.recipe(output="res://assets/generated/wrong.scn", success=False)

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

    def test_host_resource_and_geometry_limits(self):
        self.recipe('cooker.material({}); ' + BOX, limits={"max_resources": 1}, success=False)
        self.recipe(limits={"max_vertices": 3}, success=False)
        self.recipe('local b=cooker.primitive("box",{}); return cooker.scene({{asset=b},{asset=b}})',
                    limits={"max_instances": 1}, success=False, output="res://assets/generated/too-many.scn")

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
                       {"limits": {"time_ms": 2001}}, {"limits": {"memory_mb": 0}},
                       {"limits": {"extra": 2}}, {"parameters": []}, {"parameters": {"v": None}},
                       {"inputs": {"bad": "res://recipe-0.luau"}}, {"inputs": {"bad": "user://secret.res"}},
                       {"unknown": 1}, {"output": "res://assets/generated/../escape.res"},
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

    def test_output_size_failure_removes_only_its_temporary_file(self):
        directory = self.project / "assets/generated"
        directory.mkdir(parents=True)
        name = base64.b64encode(random.Random(42).randbytes(1400000)).decode()
        asset = directory / "large.tres"
        asset.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\nresource_name="' + name + '"\n')
        digest = hashlib.sha256(asset.read_bytes()).hexdigest()
        self.recipe('return cooker.input("large")', inputs={"large": "res://assets/generated/large.tres"},
                    limits={"output_mb": 1}, success=False)
        self.assertEqual(hashlib.sha256(asset.read_bytes()).hexdigest(), digest)

    def test_symlink_input_escape_is_rejected(self):
        directory = self.project / "assets/generated"
        directory.mkdir(parents=True)
        with tempfile.TemporaryDirectory(prefix="veya-recipe-outside-") as outside:
            asset = Path(outside) / "outside.tres"
            asset.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\n')
            try:
                (directory / "link.tres").symlink_to(asset)
            except OSError:
                self.skipTest("creating symlinks requires platform permission")
            self.recipe('return cooker.input("escape")', inputs={"escape": "res://assets/generated/link.tres"}, success=False)

    def test_invalid_instance_transforms_and_dense_arrays(self):
        for item in ('{asset=b,scale={0,1,1}}', '{asset=b,basis={{1,0,0},{0,1,0},{0,0,1}},scale={1,1,1}}',
                     '{asset=b,basis={{1,0,0},{2,0,0},{0,0,1}}}', '{asset=b,position={1,2}}',
                     '{asset=b,position={[1]=1,[3]=3}}', '{asset=b,position={1,2,3,named=1}}'):
            with self.subTest(item=item):
                self.recipe('local b=cooker.primitive("box",{}); return cooker.scene({' + item + '})',
                            output=f"res://assets/generated/bad-transform-{self.index}.scn", success=False)

    @unittest.skipUnless(REFERENCE, "pass --reference-godot for independent packed-resource verification")
    def test_pack_loads_in_empty_godot_project_with_full_transforms(self):
        self.recipe('''local b=cooker.primitive("box",{size={2,3,4}})
local m=cooker.material({color={0.2,0.4,0.6},roughness=0.37,metallic=0.72})
return cooker.scene({
 {asset=b,material=m,position={2,3,4},basis={{0,2,0},{-3,0,0},{0,0,4}}},
 {asset=b,material=m,position={5,6,7},scale={2,3,4}},
 {asset=b,position={0,0,0}}
})''', output="res://assets/generated/checked.scn")
        pack = "content/releases/checked.pck"
        self.job({"operation": "pack", "files": ["res://assets/generated/checked.scn"], "output": "res://" + pack})
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
    var scene = load("res://assets/generated/checked.scn").instantiate()
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
                              "--", str(self.project / pack)], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("COOKER_RECIPE_PACK_OK", run.stdout)
        self.assertNotIn("ERROR:", run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining])
# /*>>----- VEYA_COOKER */
