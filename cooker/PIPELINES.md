# Cooker Luau pipeline API v1

A production asset build is a project-local UTF-8 `.pipeline.luau` file executed
directly by Cooker:

```bash
native/third_party/godot/bin/veya_cooke --path . \
  --pipeline res://native/third_party/godot/cooker/examples/bridge.pipeline.luau
```

The script returns one dense array of job tables. It may use deterministic Luau
control flow to reduce repetition, but has no filesystem, process, network,
clock, random, `require`, bytecode-loading or engine-object API. The VM is limited
to 16 MiB, 1 second, 256 steps, depth 16 and 16,384 converted values. Cooker
converts the returned tables to native jobs and executes them sequentially.

```luau
local asset = "res://assets/generated/example/r1/example.scn"
return {
    {name="import", operation="import-scene", source="res://assets/source/example.glb",
        output=asset, type="PackedScene"},
    {name="validate", operation="validate-resource", source=asset, type="PackedScene"},
    {name="manifest", operation="asset-manifest",
        output="res://assets/generated/example/r1/asset.manifest.json",
        asset_id="example", revision=1, kind="model", entry="example.scn",
        files={"example.scn"}, source="ai-task-id", generator="model-service"},
}
```

All native job operations listed in [the Cooker README](README.md) are supported.
Humanoid animation production is described by project Lua as a `make-bone-map`
step followed by one or more `retarget-animations` steps. The latter enumerates
only `.fbx` files directly inside its declared `source_dir`, sorts them, rejects
existing outputs, and writes one library for each file in `output_dir`. This
allows hundreds of clips without exceeding the pipeline's 256-step limit.
Optional `include` and `exclude` filename arrays make selection explicit.
`animation_name`, `in_place`, and `loop` apply uniformly to that step's clips.
Two pipeline-only metadata fields are removed before native dispatch:

- `name`: optional report label.
- `if_missing`: when true, validate the declared output path and skip the step if
  its immutable output already exists. It requires `output`. This is intended for
  rebuilding dependency graphs whose published revisions cannot be overwritten.

`asset-manifest` is the native publication operation. It accepts `output`,
`asset_id`, positive integer `revision`, `kind`, `entry`, and a nonempty `files`
array, with optional `source`, `generator` and `provenance`. Every listed file
must be a cooked `.scn` or `.res` next to the manifest. Cooker computes bytes and
SHA-256 and atomically writes `asset.manifest.json`; Lua describes the identity
and provenance, while Python/PowerShell never serialize production content.

Pipelines are sequential rather than globally transactional. Each individual
recipe, manifest and PCK publication is immutable/atomic, but a later failure
does not delete earlier successful outputs. Use a new revision for changed
content. The final report includes the pipeline source hash and every completed,
skipped or failed step.

Standalone JSON `--job` remains available for low-level tests and diagnostics.
It is not an allowed canonical production description.
