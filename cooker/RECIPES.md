# Cooker Luau recipe API v2

Each `run-recipe` job compiles UTF-8 source with the pinned Luau 0.738 compiler,
runs one independent VM, and returns exactly one opaque asset handle. Native
builders create resources. Luau never sees a Node, Resource pointer, RID, ECS
entity or OS API. This is separate from Veya's behavior VM and is not registered
as a Godot ScriptLanguage. No Python/external Lua interpreter runs a recipe.

## Example

After building Cooker, the production example runs through its Luau pipeline
from the **Veya repository root**:

```bash
native/third_party/godot/bin/veya_cooke --path . \
  --pipeline res://native/third_party/godot/cooker/examples/bridge.pipeline.luau
```

Windows uses `veya_cooke.exe`. In a standalone workspace, copy the example
`.luau` files and adjust their source paths; the workspace needs `project.godot`.
Choose a new output/revision on subsequent runs: existing assets are never
overwritten. See [Pipeline API](PIPELINES.md). Standalone JSON jobs below document
the low-level test/debug protocol, not the canonical production entry.

The example computes a curved bridge deck as indexed triangles and positions
railings. Repeated mesh/material pairs become MultiMesh batches. It is a reusable
asset, not game-world placement, an AI model call or a generated picture.

## Job

```json
{
  "operation": "run-recipe",
  "source": "res://recipes/bridge.luau",
  "output": "res://assets/generated/bridge/r1/bridge.scn",
  "seed": 42,
  "parameters": {"span": 8, "segments": 24},
  "inputs": {},
  "limits": {"time_ms": 1500, "max_instances": 2000}
}
```

- Source: 1..65536 bytes of UTF-8 without NUL, `.luau` extension. External bytecode
  is rejected. Type annotations are accepted; no static analyzer runs here.
- Output: `res://assets/generated/.../*.scn` for PackedScene or `*.res` for a
  Mesh/Material. Existing targets, traversal and symlink escapes fail.
- Parameters: Lua table or low-level JSON object, <= 64 KiB serialized, depth <= 16 and <= 4096 entries
  per container. Values may be finite numbers, strings, booleans, arrays and
  objects, without nulls. Exposed recursively read-only as `cooker.parameters`.
- Inputs: <= 32 named, already cooked Mesh, Material or PackedScene resources under
  `assets/generated/`. Their resource dependency closure is checked before
  loading; shader includes are additionally resolved and checked during resource
  inspection. Combined declared dependency size is capped at 256 MiB on disk by
  default and can be lowered per job.
  Lua can access only declared
  names, not choose paths. PackedScene inputs can be measured and composed by
  Luau without exposing their Nodes. Raw model inputs are not supported; the
  existing `import-scene` operation remains the native compilation boundary.
  Input dependencies are preserved and included by subsequent `pack` jobs.
- Seed: uint32, default 1; zero normalizes to 1. Randomness uses xorshift32,
  not a platform random distribution or the clock.
- Unknown job/configuration/limit fields and coerced numeric strings fail.

Success reports include output/type/hash, source hash, input dependency hashes,
parameters, seed, effective limits, Luau/API version, handle/vertex/instance counts and VM peak
allocator bytes. Keep the report as build provenance. Cache keys should include
these inputs, limits and the Cooker binary/profile. Same-seed computation does
not promise byte-identical cross-platform Godot serialization or resource UIDs.

## APIs

Use dot calls (`cooker.mesh(...)`), not colon calls. Handles cannot be forged or
mutated. Configuration fields are read raw, never through `__index` metamethods.

| API | Contract |
| --- | --- |
| `primitive("box", {size={x,y,z}})` | Positive dimensions, default `{1,1,1}`. |
| `primitive("cylinder", {height=1,radius=0.5,top_radius=0.5,segments=24})` | Height/radius >= 0.001; top radius may be zero; integer segments 3..128. |
| `primitive("sphere", {radius=0.5,segments=24,rings=12})` | Radius >= 0.001; integer segments 4..128 and rings 2..64. |
| `material({color={r,g,b,a},roughness=0.7,metallic=0})` | StandardMaterial3D; channels/scalars 0..1, alpha optional. No transparency-mode or shader-authoring API in v1. |
| `mesh({vertices=...,indices=...,normals=...,uvs=...})` | One indexed triangle surface. Vertices are `{x,y,z}`; indices are **1-based**, clockwise. Optional normals/UVs match vertex count. Without normals the host accumulates/normalizes triangle normals; split vertices for hard edges. No tangent-generation API. |
| `scene({{asset=handle,material=material,name=...,remove=...,position=...,rotation=...,scale=...},...})` | Nonempty array of Mesh or PackedScene instances. Mesh/material pairs become MultiMesh batches. PackedScene roots must be Node3D; `name` sets the instance name and `remove` names bounded relative child paths to omit before packing. Material overrides are Mesh-only. Nodes remain opaque to Luau. |
| `input("declared_name")` | Mesh/Material handle from the input allowlist. |
| `bounds(asset)` | Mesh-local or PackedScene-root-local `{min={x,y,z},max={x,y,z}}`. PackedScene bounds include transformed MeshInstance3D and MultiMeshInstance3D descendants. |
| `random()` | Seeded xorshift32 value in `[0,1)`. |

Coordinates must be finite in `[-100000,100000]`. Arrays are dense, without named
fields. Instance position defaults to zero, Euler rotation to zero (radians,
Godot YXZ order) and scale to one. Alternatively supply
`basis={{x_column},{y_column},{z_column}}` plus position, without rotation/scale.
All columns and nonuniform scale are preserved; singular bases fail.

```luau
local box = cooker.primitive("box", {size = {1, 1, 1}})
local finish = cooker.material({color = {0.2, 0.4, 0.3}, metallic = 0.6})
local pieces = {}
for i = 1, cooker.parameters.count do
    table.insert(pieces, {asset=box, material=finish,
        position={i*1.2,0,0}, scale={1,1+cooker.random(),1}})
end
return cooker.scene(pieces)
```

## Limits and publication

Job limits may only **lower** these positive integer defaults:

| Key | Default |
| --- | --- |
| `memory_mb` | 32 MiB VM allocator memory |
| `time_ms` | 2000 ms of recipe execution |
| `max_resources` | 256 handles |
| `input_mb` | 256 MiB declared input dependency bytes |
| `max_vertices` | 500000 mesh vertices across requested handles |
| `max_instances` | 10000 scene instance declarations |
| `output_mb` | 64 MiB serialized output |

Index arrays are limited to `6 * max_vertices`. Repeated input requests consume
handle/vertex budget again. The vertex budget is not an expanded draw-vertex
budget: repeated instances use MultiMesh.

Only base/table/string/math/bit32/utf8 are opened. No os/io/debug/coroutine/require,
network, dynamic code loading, environment manipulation, print, GC controls or
native libraries are exposed. Built-in randomness is removed. Libraries, API
and parameters are read-only. Ordinary Lua errors may be caught; host validation,
memory and execution faults are sticky, even through pcall/xpcall. A broken VM
is never resumed or allowed to publish an asset.

The single result is serialized to a new temporary file beside its destination,
checked against the output byte limit, then renamed to the immutable final path.
Serialization/size failures remove only that task's temporary file. Scripts cannot
publish intermediate files. Empty output directories may remain after failure.

This is **not an OS sandbox or a total process resource guarantee**. Compilation,
input loading and saving are outside the VM time budget. A long native API cannot
be interrupted inside its C++ call. Godot allocations/decompressed input are not
charged to the VM allocator. The output byte check follows serialization. A
supervisor must own the workspace exclusively, enforce process time/memory/disk
limits, and isolate untrusted native parsers.

## Verification

`tests/test_recipes.py` invokes real Cooker jobs with PATH pointing at an empty
directory; Python is only the test harness. On 2026-09-15 macOS arm64 passed
20 recipe tests, 20 existing asset tests and 7 source/build contract tests.
Coverage includes resources, input provenance, RNG, read-only libraries, caught
faults, time/memory/geometry/output limits, malformed inputs, symlinks, immutable
outputs and independent PCK loading.

Godot 4.7.2 headless checks decode serialized MultiMesh buffers (the official
Dummy getter returns identity); a real Metal Forward+ preview also checked live
instance transforms. The bridge has 161 instances, 4 batches and 520 unique mesh
vertices. It is an integration sample, not production art or a physics/navigation
asset. Windows recipe execution, a fresh full clean engine build, and Veya's
automatic process/IPC orchestration have not been verified/implemented here.
