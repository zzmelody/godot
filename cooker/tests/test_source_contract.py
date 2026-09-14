# /*<<----- VEYA_COOKER: regression for fork boundaries and build configuration, not a runtime proof. */
import ast
import json
from pathlib import Path
import re
import runpy
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CookerSourceContract(unittest.TestCase):
    def test_profiles_disable_requested_features(self):
        profile = runpy.run_path(str(ROOT / "cooker/profile.py"))
        self.assertEqual(profile["target"], "editor")
        self.assertTrue(profile["veya_cooker"])
        for key in ("module_gdscript_enabled", "module_mono_enabled", "vulkan", "d3d12", "opengl3"):
            self.assertFalse(profile[key], key)
        for key in ("disable_physics_2d", "disable_navigation_2d", "disable_advanced_gui"):
            self.assertTrue(profile[key], key)
        for name in ("gltf", "fbx", "meshoptimizer", "vhacd", "navigation_3d", "godot_physics_3d", "basis_universal"):
            self.assertTrue(profile[f"module_{name}_enabled"], name)
        classes = json.loads((ROOT / "cooker/cooker.gdbuild").read_text())["disabled_classes"]
        self.assertIn("CanvasItem", classes)  # inherited by both Control and Node2D
        self.assertNotIn("Texture2D", classes)
        self.assertNotIn("Image", classes)

    def test_modified_source_lines_have_reason_markers(self):
        diff = subprocess.check_output(
            ["git", "diff", "--no-ext-diff", "--unified=0", "4.7.2-stable", "--"],
            cwd=ROOT, text=True, encoding="utf-8",
        )
        tracked = set()
        path = None
        new_line = 0
        for line in diff.splitlines():
            if line.startswith("+++ b/"):
                path = Path(line[6:])
                tracked.add(path)
                if path.suffix not in (".cpp", ".h", ".py", ".ps1") and path.name not in ("SCsub", "SConstruct"):
                    path = None
                if path is not None:
                    body = (ROOT / path).read_text(encoding="utf-8").splitlines()
                    marked = set()
                    depth = 0
                    for number, content in enumerate(body, 1):
                        if re.match(r"^\s*(?:# )?/\*<<----- VEYA_COOKER:", content):
                            depth += 1
                        if depth:
                            marked.add(number)
                        if re.match(r"^\s*(?:# )?/\*>>----- VEYA_COOKER", content):
                            depth -= 1
                        self.assertGreaterEqual(depth, 0, str(path))
                    self.assertEqual(depth, 0, f"unbalanced markers: {path}")
            elif line.startswith("@@ "):
                new_line = int(re.search(r"\+(\d+)", line).group(1))
            elif line.startswith("+") and path is not None:
                if line[1:].strip():
                    self.assertIn(new_line, marked, f"unmarked source change: {path}:{new_line}")
                new_line += 1
            elif line.startswith(" "):
                new_line += 1
        for path in (ROOT / "cooker").rglob("*"):
            if path.suffix in (".cpp", ".h", ".py", ".ps1") and path.relative_to(ROOT) not in tracked:
                contents = path.read_text(encoding="utf-8")
                self.assertIn("/*<<----- VEYA_COOKER:", contents, str(path))
                self.assertIn("/*>>----- VEYA_COOKER", contents, str(path))

    def test_build_scripts_parse(self):
        for path in [ROOT / "SConstruct", *ROOT.rglob("SCsub"), ROOT / "cooker/profile.py"]:
            ast.parse(path.read_text(encoding="utf-8"), filename=str(path))

    def test_windows_executable_name(self):
        tree = ast.parse((ROOT / "platform/windows/SCsub").read_text(encoding="utf-8"))
        selection = next(node for node in tree.body if isinstance(node, ast.If)
                         and isinstance(node.test, ast.Compare)
                         and isinstance(node.test.comparators[0], ast.Constant)
                         and node.test.comparators[0].value == "static_library")
        code = compile(ast.Module(body=[selection], type_ignores=[]), "windows executable selection", "exec")

        class BuildEnvironment(dict):
            def add_program(self, target, sources, PROGSUFFIX):
                return target + PROGSUFFIX

        for cooker, expected in ((True, "#bin/veya_cooke.exe"),
                                 (False, "#bin/godot.windows.editor.x86_64.exe")):
            environment = BuildEnvironment(library_type="executable", veya_cooker=cooker,
                                           PROGSUFFIX=".windows.editor.x86_64.exe")
            context = {"env": environment, "sources": []}
            exec(code, context)
            self.assertEqual(context["prog"], expected)

    def test_compile_graph_excludes_editor_and_scripts(self):
        database = ROOT / "compile_commands.json"
        if not database.exists():
            self.skipTest("Build with -CompilationDatabase first")
        files = [entry["file"].replace("\\", "/") for entry in json.loads(database.read_text())]
        prohibited = ("/modules/gdscript/", "/modules/mono/", "/editor/editor_node.cpp", "/editor/plugins/", "/editor/project_manager/", "/editor/scene/", "/scene/2d/", "/scene/gui/", "/scene/resources/2d/", "/scene/debugger/", "/renderer_rd/forward_clustered/", "/renderer_rd/forward_mobile/", "/platform/windows/display_server_windows.cpp")
        for file in files:
            for fragment in prohibited:
                self.assertNotIn(fragment, "/" + file, file)
        self.assertTrue(any(file.endswith("cooker/main.cpp") for file in files))


if __name__ == "__main__":
    unittest.main()
# /*>>----- VEYA_COOKER */
