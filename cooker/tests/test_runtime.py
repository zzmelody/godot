# /*<<----- VEYA_COOKER: real native executable tests using generated, deterministic source assets. */
import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("--cooker", type=Path, required=True)
parser.add_argument("--reference-godot", type=Path)
arguments, remaining = parser.parse_known_args()
EXECUTABLE = arguments.cooker.resolve()
REFERENCE = arguments.reference_godot.resolve() if arguments.reference_godot else None


def png(width=8, height=8):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    pixels = b"".join(b"\x00" + bytes([128, 64, 32, 255]) * width for _ in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")


def triangle_scene():
    data = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    return {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Triangle", "mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
        "bufferViews": [{"buffer": 0, "byteLength": len(data)}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}],
    }


def skinned_scene():
    scene = triangle_scene()
    payload = bytearray(base64.b64decode(scene["buffers"][0]["uri"].split(",", 1)[1]))
    def accessor(values, format_code, component_type, kind, count):
        while len(payload) % 4:
            payload.append(0)
        packed = struct.pack("<" + str(len(values)) + format_code, *values)
        view = len(scene["bufferViews"])
        scene["bufferViews"].append({"buffer": 0, "byteOffset": len(payload), "byteLength": len(packed)})
        payload.extend(packed)
        index = len(scene["accessors"])
        scene["accessors"].append({"bufferView": view, "componentType": component_type, "count": count, "type": kind})
        return index
    joints = accessor([0, 0, 0, 0] * 3, "H", 5123, "VEC4", 3)
    weights = accessor([1, 0, 0, 0] * 3, "f", 5126, "VEC4", 3)
    inverse = accessor([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], "f", 5126, "MAT4", 1)
    times = accessor([0, 1], "f", 5126, "SCALAR", 2)
    motion = accessor([0, 0, 0, 0, 1, 0], "f", 5126, "VEC3", 2)
    scene["nodes"] = [{"name": "RootBone"}, {"name": "SkinnedTriangle", "mesh": 0, "skin": 0}]
    scene["scenes"][0]["nodes"] = [0, 1]
    scene["meshes"][0]["primitives"][0]["attributes"].update({"JOINTS_0": joints, "WEIGHTS_0": weights})
    scene["skins"] = [{"joints": [0], "inverseBindMatrices": inverse}]
    scene["animations"] = [{"name": "rise", "samplers": [{"input": times, "output": motion}], "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]}]
    scene["buffers"][0] = {"byteLength": len(payload), "uri": "data:application/octet-stream;base64," + base64.b64encode(payload).decode()}
    return scene


class NativeCookerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not EXECUTABLE.is_file():
            raise RuntimeError(f"Build Cooker first: {EXECUTABLE}")
        cls.temporary = tempfile.TemporaryDirectory(prefix="veya-cooker-test-")
        cls.project = Path(cls.temporary.name)
        (cls.project / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Cooker Test"\n', encoding="utf-8")
        (cls.project / "triangle.gltf").write_text(json.dumps(triangle_scene()), encoding="utf-8")
        (cls.project / "skinned.gltf").write_text(json.dumps(skinned_scene()), encoding="utf-8")
        (cls.project / "color.png").write_bytes(png())
        textured = triangle_scene()
        textured["images"] = [{"uri": "data:image/png;base64," + base64.b64encode(png()).decode()}]
        textured["textures"] = [{"source": 0}]
        textured["materials"] = [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}]
        textured["meshes"][0]["primitives"][0]["material"] = 0
        (cls.project / "textured.gltf").write_text(json.dumps(textured), encoding="utf-8")
        textured["images"][0]["uri"] = "color.png"
        (cls.project / "external.gltf").write_text(json.dumps(textured), encoding="utf-8")
        (cls.project / "triangle.obj").write_text("o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
        (cls.project / "triangle.fbx").write_text('''; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: { Version: 1000
 Properties70: {
  P: "UpAxis", "int", "Integer", "",1
  P: "UpAxisSign", "int", "Integer", "",1
  P: "FrontAxis", "int", "Integer", "",2
  P: "FrontAxisSign", "int", "Integer", "",-1
  P: "CoordAxis", "int", "Integer", "",0
  P: "CoordAxisSign", "int", "Integer", "",1
  P: "UnitScaleFactor", "double", "Number", "",100
 }
}
Objects: {
 Geometry: 100, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
 }
 Model: 200, "Model::Triangle", "Mesh" { Version: 232
 }
}
Connections: {
 C: "OO",100,200
 C: "OO",200,0
}
''', encoding="utf-8")
        (cls.project / "forbidden_ui.tscn").write_text('[gd_scene format=3]\n[node name="UI" type="Control"]\n', encoding="utf-8")
        (cls.project / "particles.tscn").write_text('''[gd_scene load_steps=4 format=3]
[sub_resource type="ParticleProcessMaterial" id="Process"]
gravity = Vector3(0, 2, 0)
initial_velocity_min = 1.0
initial_velocity_max = 3.0
[sub_resource type="StandardMaterial3D" id="Material"]
albedo_color = Color(1, 0.2, 0, 1)
[sub_resource type="SphereMesh" id="Mesh"]
material = SubResource("Material")
radius = 0.1
height = 0.2
[node name="Fire" type="GPUParticles3D"]
amount = 128
lifetime = 2.0
process_material = SubResource("Process")
draw_pass_1 = SubResource("Mesh")
''', encoding="utf-8")
        (cls.project / "terrain.tres").write_text('''[gd_resource type="HeightMapShape3D" format=3]
[resource]
map_width = 4
map_depth = 4
map_data = PackedFloat32Array(0, 0, 0, 0, 0, 1, 2, 0, 0, 2, 3, 0, 0, 0, 0, 0)
''', encoding="utf-8")
        (cls.project / "shader_material.tres").write_text('''[gd_resource type="ShaderMaterial" load_steps=2 format=3]
[sub_resource type="Shader" id="Shader"]
code = "shader_type spatial; uniform float strength = 0.25; void fragment() { ALBEDO = vec3(strength); }"
[resource]
shader = SubResource("Shader")
shader_parameter/strength = 0.75
''', encoding="utf-8")
        (cls.project / "collider.tscn").write_text('''[gd_scene load_steps=2 format=3]
[sub_resource type="BoxShape3D" id="Shape"]
size = Vector3(2, 3, 4)
[node name="Collider" type="StaticBody3D"]
[node name="Collision" type="CollisionShape3D" parent="."]
shape = SubResource("Shape")
''', encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, *args, success=True):
        process = subprocess.run([str(EXECUTABLE), *map(str, args)], cwd=self.project, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
        if success:
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertNotIn("ERROR:", process.stdout + process.stderr)
            self.assertNotIn("leaked", process.stdout + process.stderr)
        else:
            self.assertNotEqual(process.returncode, 0, process.stdout + process.stderr)
        reports = []
        for line in process.stdout.splitlines():
            try:
                value = json.loads(line)
                if isinstance(value, dict) and "ok" in value:
                    reports.append(value)
            except ValueError:
                pass
        self.assertTrue(reports, process.stdout + process.stderr)
        self.assertEqual(reports[-1]["ok"], success)
        return reports[-1]

    def job(self, payload, success=True):
        path = self.project / "job.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return self.invoke("--headless", "--path", self.project, "--job", path, success=success)

    def test_capabilities(self):
        report = self.invoke("--capabilities")
        self.assertEqual(report["renderer"], "dummy")
        self.assertEqual(report["script_languages"], 0)
        for name in ("Node2D", "Control", "EditorNode", "GDScript"):
            self.assertFalse(report["classes"][name], name)
        for name in ("ArrayMesh", "PackedScene", "Skeleton3D", "Skin", "AnimationLibrary", "ShaderMaterial", "GPUParticles3D", "ParticleProcessMaterial", "NavigationMesh", "HeightMapShape3D", "PCKPacker", "Image", "Texture2D"):
            self.assertTrue(report["classes"][name], name)

    def test_import_mesh_roundtrip(self):
        result = self.job({"operation": "import-scene", "source": "res://triangle.gltf", "output": "res://assets/generated/triangle.scn"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(result["mesh_count"], 1)
        self.assertEqual(result["surface_count"], 1)
        self.assertEqual(result["vertex_count"], 3)

    def test_texture_roundtrip(self):
        for compression in ("lossless", "basisu", "s3tc", "bptc", "etc2", "astc"):
            with self.subTest(compression=compression):
                result = self.job({"operation": "import-texture", "source": "res://color.png", "output": f"res://assets/generated/{compression}.res", "compression": compression})
                self.assertEqual(result["mipmap_count"], 3)
                result = self.job({"operation": "validate-resource", "source": result["output"], "type": "PortableCompressedTexture2D"})
                self.assertEqual((result["width"], result["height"]), (8, 8))
                self.assertGreater(result["pixel_bytes"], 0)

    def test_fbx_and_obj_import(self):
        for extension in ("fbx", "obj"):
            with self.subTest(extension=extension):
                result = self.job({"operation": "import-scene", "source": f"res://triangle.{extension}", "output": f"res://assets/generated/{extension}.scn"})
                result = self.job({"operation": "validate-resource", "source": result["output"]})
                self.assertEqual(result["vertex_count"], 3)

    def test_mesh_processing_lod_uv_and_convex_decomposition(self):
        (self.project / "sphere.tres").write_text('''[gd_resource type="SphereMesh" format=3]
[resource]
radius = 1.0
height = 2.0
radial_segments = 32
rings = 16
''', encoding="utf-8")
        result = self.job({"operation": "process-mesh", "source": "res://sphere.tres", "output": "res://assets/generated/sphere.res", "lightmap_uv": True, "collision_output": "res://assets/generated/sphere_collision.scn"})
        self.assertGreater(result["lod_count"], 0)
        self.assertGreater(result["hull_count"], 0)
        saved = self.job({"operation": "validate-resource", "source": result["output"], "type": "ArrayMesh"})
        self.assertGreater(saved["vertex_count"], 0)
        self.assertEqual(saved["uv2_count"], saved["vertex_count"])
        self.assertEqual(saved["lod_count"], result["lod_count"])
        saved = self.job({"operation": "validate-resource", "source": result["collision_output"]})
        self.assertEqual(saved["collider_count"], result["hull_count"])

    def test_multimesh_transform_roundtrip(self):
        (self.project / "instances.tres").write_text('''[gd_resource type="MultiMesh" load_steps=2 format=3]
[sub_resource type="BoxMesh" id="Mesh"]
[resource]
transform_format = 1
instance_count = 2
mesh = SubResource("Mesh")
buffer = PackedFloat32Array(1,0,0,2,0,1,0,3,0,0,1,4,1,0,0,5,0,1,0,6,0,0,1,7)
''', encoding="utf-8")
        result = self.job({"operation": "save-resource", "source": "res://instances.tres", "output": "res://assets/generated/instances.res"})
        result = self.job({"operation": "validate-resource", "source": result["output"]})
        self.assertEqual(result["instance_count"], 2)
        self.assertEqual(result["positions"], [[2, 3, 4], [5, 6, 7]])

    def test_shader_include_pack(self):
        directory = self.project / "assets/generated"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "common.gdshaderinc").write_text("const float VALUE = 0.3;\n", encoding="utf-8")
        (directory / "include.gdshader").write_text('shader_type spatial;\n#include "res://assets/generated/common.gdshaderinc"\nvoid fragment() { ALBEDO = vec3(VALUE); }\n', encoding="utf-8")
        result = self.job({"operation": "pack", "files": ["res://assets/generated/include.gdshader"], "output": "res://content/releases/shader.pck"})
        self.assertEqual(set(result["files"]), {"res://assets/generated/include.gdshader", "res://assets/generated/common.gdshaderinc"})

    def test_embedded_and_external_textures(self):
        for name in ("textured", "external"):
            with self.subTest(name=name):
                result = self.job({"operation": "import-scene", "source": f"res://{name}.gltf", "output": f"res://assets/generated/{name}.scn"})
                result = self.job({"operation": "validate-resource", "source": result["output"]})
                self.assertEqual(result["material_texture_count"], 1)
                self.assertGreater(result["material_texture_bytes"], 0)

    def test_path_and_native_dependency_rejection(self):
        for path in ("res://../escape.res", "user://escape.res", "res://content/escape.res", "res://assets/generated/../../../escape.res"):
            with self.subTest(path=path):
                self.job({"operation": "save-resource", "source": "res://terrain.tres", "output": path}, success=False)
        (self.project / "native.gdextension").write_text('[configuration]\nentry_symbol="forbidden"\n', encoding="utf-8")
        (self.project / "native.tres").write_text('''[gd_resource type="Resource" load_steps=2 format=3]
[ext_resource type="GDExtension" path="res://native.gdextension" id="1"]
[resource]
metadata/native = ExtResource("1")
''', encoding="utf-8")
        self.job({"operation": "validate-resource", "source": "res://native.tres"}, success=False)

    def test_pack_dependency_closure(self):
        material = "res://assets/generated/pack_material.res"
        self.job({"operation": "save-resource", "source": "res://shader_material.tres", "output": material})
        scene = "res://assets/generated/pack_scene.tscn"
        (self.project / scene.removeprefix("res://")).write_text('''[gd_scene load_steps=3 format=3]
[ext_resource type="Material" path="res://assets/generated/pack_material.res" id="Material"]
[sub_resource type="BoxMesh" id="Mesh"]
[node name="Box" type="MeshInstance3D"]
mesh = SubResource("Mesh")
material_override = ExtResource("Material")
''', encoding="utf-8")
        result = self.job({"operation": "pack", "files": [scene], "output": "res://content/releases/closure.pck"})
        self.assertEqual(set(result["files"]), {scene, material})
        self.assertFalse(result["signed"])
        self.assertEqual(len(result["sha256"]), 64)
        duplicate = self.job({"operation": "pack", "files": [scene], "output": "res://content/releases/closure2.pck"})
        self.assertEqual(result["sha256"], duplicate["sha256"])
        self.job({"operation": "pack", "files": [scene], "output": "res://content/releases/closure.pck"}, success=False)
        self.job({"operation": "pack", "files": ["res://shader_material.tres"], "output": "res://content/releases/forbidden.pck"}, success=False)

    @unittest.skipUnless(REFERENCE, "Pass --reference-godot for independent engine/PCK verification")
    def test_reference_engine_pack_load(self):
        resources = []
        for name, source, operation, extension in (("model", "res://textured.gltf", "import-scene", "scn"), ("particles", "res://particles.tscn", "save-resource", "scn"), ("terrain", "res://terrain.tres", "save-resource", "res"), ("shader", "res://shader_material.tres", "save-resource", "res")):
            result = self.job({"operation": operation, "source": source, "output": f"res://assets/generated/reference_{name}.{extension}"})
            resources.append(result["output"])
        pack = self.job({"operation": "pack", "files": resources, "output": "res://content/releases/reference.pck"})
        with tempfile.TemporaryDirectory(prefix="veya-cooker-reference-") as directory:
            project = Path(directory)
            (project / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Independent Resource Check"\n', encoding="utf-8")
            # This script belongs only to the full reference engine; it is never shipped in Cooker.
            script = project / "verify.gd"
            script.write_text('''extends SceneTree
func _initialize():
    assert(ProjectSettings.load_resource_pack(OS.get_cmdline_user_args()[0]))
    var model = load("res://assets/generated/reference_model.scn").instantiate()
    var meshes = model.find_children("*", "MeshInstance3D", true, false)
    assert(meshes.size() == 1)
    assert(meshes[0].mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size() == 3)
    var pixels = meshes[0].mesh.surface_get_material(0).albedo_texture.get_image()
    assert(pixels.get_width() == 8 and pixels.get_height() == 8)
    assert(pixels.get_pixel(0, 0).is_equal_approx(Color8(128, 64, 32, 255)))
    model.free()
    var particles = load("res://assets/generated/reference_particles.scn").instantiate()
    assert(particles.amount == 128 and particles.lifetime == 2.0)
    assert(particles.process_material.gravity == Vector3(0, 2, 0))
    particles.free()
    var heights = load("res://assets/generated/reference_terrain.res")
    assert(heights.map_data[10] == 3.0)
    var material = load("res://assets/generated/reference_shader.res")
    assert(material.get_shader_parameter("strength") == 0.75)
    print("REFERENCE_PACK_LOAD_OK")
    quit(0)
''', encoding="utf-8")
            pack_path = self.project / pack["output"].removeprefix("res://")
            process = subprocess.run([str(REFERENCE), "--headless", "--path", str(project), "--script", str(script), "--", str(pack_path)], cwd=project, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertIn("REFERENCE_PACK_LOAD_OK", process.stdout)
            self.assertNotIn("ERROR:", process.stdout + process.stderr)

    def test_skin_animation_import(self):
        result = self.job({"operation": "import-scene", "source": "res://skinned.gltf", "output": "res://assets/generated/skinned.scn"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(result["vertex_count"], 3)
        self.assertGreaterEqual(result["bone_count"], 1)
        self.assertGreaterEqual(result["animation_count"], 1)

    def test_heightmap_shape_roundtrip(self):
        result = self.job({"operation": "save-resource", "source": "res://terrain.tres", "output": "res://assets/generated/terrain.res"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "HeightMapShape3D"})
        self.assertEqual((result["width"], result["depth"], result["sample_count"]), (4, 4, 16))

    def test_shader_parameter_roundtrip(self):
        result = self.job({"operation": "save-resource", "source": "res://shader_material.tres", "output": "res://assets/generated/shader_material.res"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "ShaderMaterial"})
        self.assertEqual(result["shader_parameters"]["strength"], 0.75)

    def test_collider_roundtrip(self):
        result = self.job({"operation": "save-resource", "source": "res://collider.tscn", "output": "res://assets/generated/collider.scn"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(result["collider_count"], 1)

    def test_navigation_bake(self):
        # Godot faces are clockwise; add_faces reverses indices for Recast internally.
        faces = [[-5, 0, -5], [5, 0, 5], [-5, 0, 5], [-5, 0, -5], [5, 0, -5], [5, 0, 5]]
        result = self.job({"operation": "bake-navigation", "vertices": faces, "output": "res://assets/generated/navigation.res"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "NavigationMesh"})
        self.assertGreater(result["polygon_count"], 0)
        self.assertGreater(result["vertex_count"], 0)

    def test_missing_resource_fails(self):
        self.job({"operation": "validate-resource", "source": "res://missing.res"}, success=False)

    def test_ui_asset_fails_instead_of_becoming_placeholder(self):
        self.job({"operation": "validate-resource", "source": "res://forbidden_ui.tscn"}, success=False)

    def test_particle_asset_roundtrip(self):
        result = self.job({"operation": "save-resource", "source": "res://particles.tscn", "output": "res://assets/generated/particles.scn", "type": "PackedScene"})
        result = self.job({"operation": "validate-resource", "source": result["output"], "type": "PackedScene"})
        self.assertEqual(result["root_type"], "GPUParticles3D")
        self.assertEqual(result["particles"], {"amount": 128, "lifetime": 2.0, "gravity": [0, 2, 0]})

    def test_unknown_operation_fails(self):
        self.job({"operation": "run-script"}, success=False)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining])
# /*>>----- VEYA_COOKER */
