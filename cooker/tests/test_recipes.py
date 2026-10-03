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
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20, env=environment)
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
        return result

    def test_capabilities_and_no_external_runtime(self):
        run = subprocess.run([str(EXECUTABLE), "--capabilities"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
        report = json.loads(next(line for line in run.stdout.splitlines() if line.startswith('{')))
        self.assertEqual(report["script_languages"], 0)
        self.assertEqual(report["recipe_runtime"]["version"], "0.738")
        self.assertEqual(report["recipe_runtime"]["api_version"], 6)
        for name in ("field", "sample", "field_info", "points"):
            self.assertIn(name, report["recipe_runtime"]["apis"])
        self.assertIn("scene", report["recipe_runtime"]["apis"])
        self.assertIn("procedural_texture", report["recipe_runtime"]["apis"])
        self.assertIn("effect", report["recipe_runtime"]["apis"])
        self.assertEqual(report["pipeline_runtime"]["language"], "Luau")
        self.assertEqual(report["pipeline_runtime"]["api_version"], 1)
        # All recipe jobs below run with PATH pointing at an empty/nonexistent directory.
        report = self.recipe()
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
            'return cooker.points({kinds={"a"},points={}})',
        ):
            with self.subTest(code=code[:60]):
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

    def test_input_shader_include_provenance(self):
        directory = self.project / "content/worlds/fixture/generated"
        directory.mkdir(parents=True)
        (directory / "color.gdshaderinc").write_text('const vec3 RECIPE_TINT = vec3(0.3,0.4,0.5);\n')
        (directory / "surface.gdshader").write_text('shader_type spatial;\n#include "res://content/worlds/fixture/generated/color.gdshaderinc"\nvoid fragment() { ALBEDO = RECIPE_TINT; }\n')
        (directory / "finish.tres").write_text('[gd_resource type="ShaderMaterial" load_steps=2 format=3]\n[ext_resource type="Shader" path="res://content/worlds/fixture/generated/surface.gdshader" id="1"]\n[resource]\nshader=ExtResource("1")\n')
        result = self.recipe('return cooker.input("finish")', inputs={"finish": "res://content/worlds/fixture/generated/finish.tres"})
        self.assertEqual(set(result["input_sha256"]), {"res://content/worlds/fixture/generated/" + name for name in
                                                     ("finish.tres", "surface.gdshader", "color.gdshaderinc")})

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
                    limits={"max_instances": 1}, success=False, output="res://content/worlds/fixture/generated/too-many.scn")

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

    def test_output_size_failure_removes_only_its_temporary_file(self):
        directory = self.project / "content/worlds/fixture/generated"
        directory.mkdir(parents=True)
        name = base64.b64encode(random.Random(42).randbytes(1400000)).decode()
        asset = directory / "large.tres"
        asset.write_text('[gd_resource type="StandardMaterial3D" format=3]\n[resource]\nresource_name="' + name + '"\n')
        digest = hashlib.sha256(asset.read_bytes()).hexdigest()
        self.recipe('return cooker.input("large")', inputs={"large": "res://content/worlds/fixture/generated/large.tres"},
                    limits={"output_mb": 1}, success=False)
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
