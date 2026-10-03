# Cooker Luau recipe API v4

Veya GI additions: `atmosphere` accepts `sdfgi_enabled`, `sdfgi_cascades`
(4/6/8), `sdfgi_cell_size` (.25..8), `sdfgi_occlusion`, `sdfgi_read_sky`,
`sdfgi_bounce` (0...5), `sdfgi_energy` (0..2), and normal/probe bias (.1..4).
Defaults leave GI disabled. Keep the same cascade layout across day presets.

`scene` mesh entries with `lightmap=true` create named MeshInstance3D geometry
instead of MultiMesh batching. Other entries retain the existing batching.
`lightmap_scene({scene=handle, lights={...}, quality=2, bounces=3,
directional=true, interior=true, probes=2, max_texture_size=2048,
texel_size=.15})` returns an unbaked PackedScene for `bake-lightmap`.
Quality is 0..3; probes is 1..3 (4/8/16 subdivisions); atlas size is a power of
two from 256..4096. Each of at most 32 fixed omni lamps has `position`, `color`,
`energy` (0..16), `range` (.1..128), `size` (0..2). Environment light is disabled
during this bake; changing sun/moon belongs to runtime SDFGI. The native baker
creates UV2 using each mesh's full transform, removes bake-only lamps and embeds
HDR texture arrays and dynamic-object probes in the output scene.

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
  "output": "res://content/worlds/example/generated/bridge/bridge.scn",
  "seed": 42,
  "parameters": {"span": 8, "segments": 24},
  "inputs": {},
  "limits": {"time_ms": 1500, "max_instances": 2000}
}
```

- Source: 1..65536 bytes of UTF-8 without NUL, `.luau` extension. External bytecode
  is rejected. Type annotations are accepted; no static analyzer runs here.
- Output: `res://content/worlds/<id>/generated/.../*.scn` (or the `mod-worlds` and
  `content/shared/generated/` roots) for PackedScene or `*.res` for a
  Mesh/Material/Texture2D. Existing targets, traversal and symlink escapes fail.
- Parameters: Lua table or low-level JSON object, <= 64 KiB serialized, depth <= 16 and <= 4096 entries
  per container. Values may be finite numbers, strings, booleans, arrays and
  objects, without nulls. Exposed recursively read-only as `cooker.parameters`.
- Inputs: <= 32 named, already cooked Mesh, Material, Texture2D, Shader or PackedScene resources under
  one of those `generated/` roots. Their resource dependency closure is checked before
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
| `material({color={r,g,b,a},roughness=0.7,metallic=0,albedo_texture=t,...})` | StandardMaterial3D. Color/scalar channels 0..1, alpha optional. Optional Texture2D handles from `input()` or `procedural_texture`: `albedo_texture`, `normal_texture` (enables normal mapping; `normal_scale` 0..4, default 1), `orm_texture` (AO/roughness/metallic on R/G/B), `emission_texture`, `heightmap_texture` (`heightmap_scale` 0..16, default 1). `uv_scale` 0.1..64 (default 1), `triplanar` boolean, `transparency` `disabled` or `alpha`. Omitted texture slots keep the v3 scalar-only material. No shader-authoring API. |
| `procedural_texture({...})` | Deterministic CPU RGBA texture. `kind` is `radial`, `ring`, `spark`, `streak`, or `soft-noise`; `size` is a power of two from 32 through 1024. Bounded fields include color, secondary, seed, radius, width, softness, angle and noise scale. |
| `effect({name=...,mode=...,duration=...,layers={...}})` | Script-free PackedScene with one to four `billboard_particles`, `animated_sprite`, or `simple_mesh` layers. It accepts only typed motion/material/curve/gradient fields; no shader source, Node pointer, method track, collision, audio, light, camera, environment, filesystem or network surface exists. |
| `mesh({vertices=...,indices=...,normals=...,uvs=...})` | One indexed triangle surface. Vertices are `{x,y,z}`; indices are **1-based**, clockwise. Optional normals/UVs match vertex count. Without normals the host accumulates/normalizes triangle normals; split vertices for hard edges. No tangent-generation API. |
| `scene({{asset=handle,material=material,name=...,remove=...,position=...,rotation=...,scale=...},...})` | Nonempty array of Mesh or PackedScene instances. Mesh/material pairs become MultiMesh batches. PackedScene roots must be Node3D; `name` sets the instance name and `remove` names bounded relative child paths to omit before packing. Material overrides are Mesh-only. Nodes remain opaque to Luau. |
| `input("declared_name")` | Mesh/Material/Texture2D/Shader/PackedScene handle from the input allowlist. Texture2D enables licensed cooked source textures in typed effects and PBR `material()` slots; Shader is accepted only by the separate typed atmosphere builder. |
| `camera_attributes({...})` | CameraAttributesPractical with bounded fixed exposure, near/far DOF and typed Veya camera-profile metadata. Configuration is authored in Luau; there are no Nodes or frame callbacks. |
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

Effect recipes additionally enforce a hard ceiling of four layers, four particle
systems, 32 nodes, 512 particles per system, 1024 declared particles total and
four source textures. One-shot effects and particles are capped at five seconds;
looping/ambient particles are capped at ten seconds. A particle layer supports
point/sphere/sphere-surface/box/ring emission, direction/spread, velocity,
gravity, damping, rotation, color gradients, alpha/scale curves, blend mode and
an explicit visibility AABB. `animated_sprite` uses property tracks only.
Every particle system receives a deterministic fixed seed and 30 Hz simulation.

## Authored atmosphere treatment

`cooker.atmosphere` also accepts optional bounded scene treatment fields. Omitted
fields keep the original atmosphere profiles' effects disabled. All parameters
and preset choices are authored in project Luau; this builder only validates and
serializes an `Environment` and its sky/sun metadata.

Atmosphere v5 adds `contrast` [0.8,1.25], `saturation` [0.6,1.3], `brightness`
[0.8,1.2] (all default 1), and `ssr_enabled` (default false). Adjustments remain
off when all three color controls equal 1; AgX remains the tone mapper.

`camera_attributes` requires a bounded `preset_id`. It stores `distance` [2.3,8],
`shoulder_offset` [-1.2,1.2], `target_height` [0.8,2.2], `zoom_smoothing` [1,30],
`fov` [25,100], `run_fov` [25,110], three smoothing rates [0.1,30], `clip_near`
[0.02,1] and `clip_far` [16,4000] as `veya_camera_profile` metadata. Orientation
belongs to ECS and is preserved on selection. Fixed `exposure_multiplier`
is [0.25,4]; automatic exposure stays disabled. DOF fields are `dof_far_enabled`,
`dof_far_distance` [0.1,256], `dof_far_transition` [0.1,128], `dof_near_enabled`,
`dof_near_distance`/`dof_near_transition` [0.1,16], and `dof_amount` [0,0.3].

| Fields | Bounds |
| --- | --- |
| `cloud_speed`, `cloud_wind` | 0..0.05 noise units/second (default 0), nonzero horizontal direction (default `{0.8,0,0.6}`; normalized by Cooker) |
| `fog_height`, `fog_height_density` | -1000..1000 metres, 0..1 |
| `fog_aerial_perspective`, `fog_sun_scatter` | 0..1 |
| `volumetric_density`, `volumetric_length` | 0..0.05, 16..512 metres |
| `volumetric_albedo` | RGB channels 0..1 |
| `volumetric_anisotropy`, `volumetric_ambient_inject` | -0.9..0.9, 0..1 |
| `ssao_intensity`, `ssao_radius` | 0..2, 0.1..4 metres |
| `ssil_intensity`, `ssil_radius` | 0..2, 0.5..16 metres |
| `glow_intensity`, `glow_threshold` | 0..1, 1..8 |

Zero density/intensity disables the corresponding effect. Volumetric fog never
obscures the sky. These effects require validation in packaged Veya's target
renderer; Cooker does not render them. Atmosphere recipes do not create or own
lights: the authored world must use the matching sun metadata.

The builder stores horizontal `cloud_drift` in the ShaderMaterial and uses Sky's
automatic process mode with 256-pixel radiance faces. A shader using `TIME`
selects realtime radiance filtering; this face size does not cap the resolution
of the visible sky. The project sky shaders render their background at full
resolution and use a continuous 900-second drift cycle, matching TIME rollover.

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
