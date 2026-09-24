# Veya Asset Cooker

This is the native asset-only build of the `4.7.2-stable` source, on branch
`veya-asset-cooker`. The normal Godot editor/template paths remain separate.

## Scope

`profile.py` retains `target=editor` and a consistent `TOOLS_ENABLED` ABI across
the executable. `veya_cooker=yes` selects the native entry and an explicit
importer library instead of `EditorNode`, the project manager, editor plugins,
desktop preferences, external Blender/FBX2glTF runners, and scene previews.
`cooker.gdbuild` disables 2D/UI authoring classes, not image/texture resources
needed by 3D materials. GDScript/.NET, Input/InputMap, audio, DisplayServer,
CameraServer/capture, text/theme, movie writing/playback and real rendering
backends are disabled. Camera3D, Area3D and LightmapGI remain serializable scene
data; they do not restore live camera capture, audio, display or GPU baking.

`run-recipe` embeds a separate, statically linked Luau 0.738 compiler/VM for
asset computation. It is not a Godot ScriptLanguage or Veya's ECS behavior VM;
`script_languages` remains zero. Jobs need no external Python/Lua interpreter.
See [Recipe API](RECIPES.md).

`--pipeline` embeds a second bounded Luau use for the canonical production step
graph. It returns declarative native jobs and cannot perform IO itself. Import,
recipe, validation, manifest and PCK parameters therefore live in project Lua,
not Python/PowerShell or JSON job generators. See [Pipeline API](PIPELINES.md).

The previously verified Windows x64 build and current macOS arm64 build pass 20
native asset/error tests; the service-removal revision has been rebuilt on macOS
only. The Luau recipe/pipeline suite passes 23 tests and the current source/build
contract suite contains seven tests. `core/input`,
`scene/gui`, `scene/2d`, `scene/resources/2d`, `scene/audio`, `scene/theme`, scene
debugger, all audio/camera/display/text/movie server implementations, editor UI,
GDScript/.NET, Metal and other real rendering implementations are excluded from
the Cooker compilation graph. Private Node/Viewport declarations needed for 3D
scene/resource serialization remain; no UI, input, audio or display service is
registered.
The Dummy backend preserves mesh and texture data; the cooker adds CPU 3D
MultiMesh transform access instead of the upstream dummy identity result.

## Windows build

The selected Luau source directories must be clean at the commit in
`cooker/luau.json`. In Veya, the existing `../luau` submodule supplies this checkout;
the build never updates it. For a standalone Cooker clone, prepare a checkout of
`https://github.com/luau-lang/luau.git` at
`c54f558b4d5748ab0658610b8ce0c432053e41eb`. Set `veya_luau_source=/absolute/path`
in SCons, `-LuauSource` in PowerShell, or `--luau-source` in the macOS wrapper.
Only Common, Ast, Bytecode, Compiler and VM are linked, not CLI, analyzer or JIT.
Ship Luau's `LICENSE.txt` and `lua_LICENSE.txt` with a distributed Cooker binary.

Use a Python environment containing SCons 4.9.1, and Visual Studio C++ tools:

```powershell
./cooker/build.ps1 -SConsPython F:/creative/build/cooker-venv/Scripts/python.exe -Jobs 12 -CompilationDatabase
```

The build script explicitly passes `d3d12=no` and `arch=x86_64`: upstream Windows
platform defaults override the Python profile for these options. No dependency
revision is fetched or changed by this script. `lto=none` is the default for
initial verification; `-Lto full` is a separate size optimization experiment.

The Windows executable is `bin/veya_cooke.exe`. SCons links this short name
directly; ordinary Godot builds retain their original artifact names.

## macOS build

Use a native Python/SCons installation and Xcode Command Line Tools:

```bash
./cooker/build.sh --jobs 10 --compiledb
```

The script selects the host architecture (`arm64` or `x86_64`) by default; use
`--arch` only for an intentional cross-architecture build. `VEYA_SCONS` may name
an alternate SCons executable. The unbundled command-line result is
`bin/veya_cooke`. Cooker builds compile only the macOS filesystem, crash handler,
headless OS loop and CLI entry from `platform/macos`; AppKit display servers,
embedded editor support, native menus, TTS, CoreAudio/CoreMIDI and Metal drivers
stay out of the build graph. The remaining Cocoa/AppKit linkage belongs to the
macOS process/filesystem OS layer, not a Godot DisplayServer. Ordinary macOS
editor/template builds retain their upstream source list and application bundle
behavior.

## Native batch protocol (version 1)

```text
veya_cooke[.exe] --capabilities
veya_cooke[.exe] --path PROJECT --pipeline res://path/to/asset.pipeline.luau
veya_cooke[.exe] --path PROJECT --job JOB.json
```

`PROJECT` must contain `project.godot`. There is no generic engine-script entry,
editor window, preview renderer, or interactive game loop. The process sets a nonzero exit code
on failure and emits a JSON result. Production graphs use the sandboxed Luau
pipeline entry; see [Pipeline API](PIPELINES.md). The JSON job entry is retained
only as a low-level test/debug protocol. Jobs currently implemented in source:

- `import-scene`: `source`, `output`, optional `type` (`PackedScene`, `ArrayMesh`,
  `AnimationLibrary`, `MeshLibrary`), and native importer `options`.
- `validate-resource`: `source`, optional expected `type`.
- `save-resource`: load/validate a native text or binary resource, then save it
  to `output`; supports declarative 3D/VFX scenes without a script generator.
- `import-texture`: `source`, `output`, `compression` (`lossless`, `basisu`,
  `s3tc`, `bptc`, `etc2`, `astc`), optional `mipmaps` and `normal_map`.
- `process-mesh`: `source` is a Mesh resource (including declarative primitive
  meshes); `output` is an ArrayMesh. Optimizes indices, generates LODs and a
  shadow mesh. Optional `lightmap_uv` unfolds UV2 before generating LODs;
  `collision_output` saves V-HACD convex hulls as a 3D collision scene.
- `bake-navigation`: `vertices` contains clockwise triangle face coordinates;
  `output` stores a NavigationMesh baked by CPU Recast. Optional `cell_size`,
  `cell_height`, `agent_height`, `agent_radius`, `agent_max_climb`.
- `pack`: `files` contains cooked resource roots; `output` stores an unsigned
  PCK with sorted dependency closure, including Shader includes. The result
  supplies SHA-256 hashes. It does not sign or mount a release into Veya.
- `run-recipe`: `source` is UTF-8 `.luau`, `output` is an immutable `.scn` or
  `.res`; optional `parameters`, `seed`, declared `inputs` and lowered `limits`.
  Generates mesh/material/MultiMesh resources and measures/composes declared,
  already-cooked PackedScene inputs. See [Recipe API](RECIPES.md).
- `asset-manifest`: atomically publishes `asset.manifest.json` from identity,
  revision, provenance and cooked `.scn`/`.res` files declared by a Luau
  pipeline; Cooker supplies byte sizes and SHA-256 hashes.
- `make-bone-map`: creates a Godot `BoneMap` `.res` from a Luau-declared
  `SkeletonProfileHumanoid` mapping and checks required bones.
- `retarget-animations`: imports a bounded FBX directory into one standard
  `AnimationLibrary` `.res` per source, using a declared BoneMap. It supports
  explicit include/exclude lists, one selected animation, optional horizontal
  in-place conversion and looping. Tracks are named `%GeneralSkeleton:Bone`.
- `retarget-model`: imports a skinned FBX as a `.scn` through the same Godot
  skeleton mapping and rest correction path.
- `make-animation-preview`: combines a cooked model and animation library into
  a `.scn` for target-runtime visual checks.

Images embedded in GLTF/FBX are currently embedded uncompressed in the cooked
resource: the batch importer cannot schedule editor filesystem reimports.
Texture compression jobs use `PortableCompressedTexture2D` and retain their
compressed data for saving. Direct image jobs accept the image formats retained
by the profile; model import accepts GLTF/GLB, FBX via ufbx and OBJ. Blender and
external FBX converters are not started.

All asset paths use `res://` inside the specified workspace. Resource outputs
belong under `assets/generated/`; PCK outputs under `content/releases/` and may
not overwrite an existing pack. Pack dependencies must already be cooked under
`assets/generated/`; raw image/model import sources are not packed. Native
extensions and post-import scripts are rejected. External `_subresources`
overrides are not exposed by this first protocol; they require a declared output
manifest. Raw data resources can still be authored as `.tres`/`.tscn`.

This is a batch compiler, not a hostile-input sandbox. Run it in a supervisor-
owned staging workspace, set process memory/time limits, and do not allow the
workspace to be modified concurrently. Native importer parsers and shader
preprocessing can access their referenced input files. No GPU compilation,
visual validation, lightmap rendering, GPU particle simulation or screenshot
generation is claimed. UV2 unfolding and navigation baking are CPU operations.
The full editor/template paths are preserved by conditional compilation. The
Cooker target is functionally tested on Windows x64 and macOS arm64; macOS
x86_64 remains available as a build selection but is not yet runtime-verified.
Other desktop platforms are not build-verified.

## Verification

```powershell
python cooker/tests/test_source_contract.py
python cooker/tests/test_runtime.py --cooker bin/veya_cooke.exe --reference-godot PATH/TO/Godot_v4.7.2-stable_win64_console.exe -v
python cooker/tests/test_recipes.py --cooker bin/veya_cooke.exe --reference-godot PATH/TO/Godot_v4.7.2-stable_win64_console.exe -v
```

On macOS, use `bin/veya_cooke` and the executable inside a matching Godot 4.7.2
application bundle as `--reference-godot`.

The reference-engine check loads the produced PCK in a separate empty project,
without source art or import cache, and checks mesh vertices, texture pixels,
particle parameters, height data and shader parameters. Its GDScript test driver
runs only in the original Godot binary; it is not a Cooker dependency. The test
is explicitly skipped if `--reference-godot` is omitted. All fixtures are small,
deterministically generated test geometry/images, not visual-quality examples.
Source checks validate modification markers, SCons syntax and exclusions in
`compile_commands.json`; build with `-CompilationDatabase` to avoid skipping the
compilation-graph check. Pin the cooker executable hash and profile in downstream
cache keys. See the parent verification notes for actual build provenance.

## Source maintenance

All cooker source/configuration changes belong in this subrepository. Mark C++
changes with `/*<<----- VEYA_COOKER: reason */` / `/*>>----- VEYA_COOKER */`;
use the same markers in Python/PowerShell comments. JSON build profiles are data
and remain valid JSON. Do not remove image/UV/Texture2D support as "2D gameplay".
Do not move importer code or signing private keys into the exported Veya client.

The parent project documents reasons, integration and actual verification in
`docs/asset-cooker-fork.md`.
