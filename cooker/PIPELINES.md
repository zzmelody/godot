# Cooker Luau pipeline API v1

A production asset build is a project-local UTF-8 `.pipeline.luau` file executed
directly by Cooker:

```bash
native/third_party/godot/bin/veya_cooke --path . \
  --pipeline res://native/third_party/godot/cooker/examples/bridge.pipeline.luau
```

Development may add `--no-pack` to omit pipeline `pack` steps while keeping
resource generation and manifests unchanged. `--force` rebuilds verified cached
outputs, including per-source retarget clips. Cache fingerprints include the
Cooker executable SHA-256, request parameters and transitive resource inputs;
a toolchain change invalidates prior receipts. Production descriptions remain
the same Luau pipelines in either mode.

The script returns one dense array of job tables. It may use deterministic Luau
control flow to reduce repetition, but has no filesystem, process, network,
clock, random, `require`, bytecode-loading or engine-object API. The VM is limited
to 16 MiB, 1 second, 256 steps, depth 16 and 16,384 converted values. Cooker
converts the returned tables to native jobs and executes them sequentially.

```luau
local asset = "res://content/worlds/example/generated/example/example.scn"
return {
    {name="import", operation="import-scene", source="res://content/worlds/example/source/example.glb",
        output=asset, type="PackedScene"},
    {name="validate", operation="validate-resource", source=asset, type="PackedScene"},
    {name="manifest", operation="asset-manifest",
        output="res://content/worlds/example/generated/example/asset.manifest.json",
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
`baked_target=true` accepts a body-specific offline bake. After the normal
Humanoid axis/name import, it retains skeleton rotation tracks and Root/Hips
translation, removes object/scale/non-skeleton tracks, and requires a Hips
rotation plus at least ten joint rotations. A stationary Hips position track
may be absent after importer optimization. The flag is Boolean and participates
in verified clip cache fingerprints when enabled; existing requests with the
default `false` keep their prior fingerprints. This does not add foot contact
correction or replace skin weights. The Veya source-art workflow is documented
in [Blender retargeting](../../../../docs/blender-retarget.md).
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

`compose-character-animation` normally rejects a clip ID already present in its
`base_library`. A clip can declare `replace_existing=true` to replace that exact
existing semantic in a new output revision. The flag must be Boolean; a missing
base ID or duplicate ID in the same request is rejected. Retained animations are
shared unchanged and the base library is never written. Verified cache receipts
include this flag in their normal parameter hash.

To substitute a body bake into an existing compiled semantic catalog, the
same composer accepts `source_remap={{from="res://.../",to="res://.../"}}`
instead of `clips`. This bounded native expansion reads the base animation's
compiled source segments and mirrors/rotation offsets; it does not execute
provenance JSON. Declare `expected_clips`, `expected_replaced`, an explicit
`retained` semantic array, and `preserve_base_hips=true`. Every segment of a
replaced semantic must match a prefix. Ambiguous prefixes, partial substitution,
missing resources and unexpected retained semantics fail before composition.
Segment speed compensates for differing import endpoints to retain duration.
The original Hips position track, playback length, loop and step are preserved
exactly; body rotations come from the new baked source. Position flattening,
alignment and contact offsets already compiled in the base therefore survive.
Expanded old/new source paths enter the normal verified cache dependency hash.
`validate-character-actions` with `expected_changed_from_composition=true` and
`compare_base` checks the recorded replaced IDs, exact playback/loop values and
every Hips position key, plus the normal unchanged-clip comparison. These gates
verify resource data; packaged Veya rendering remains a separate check.

A composed looping clip can declare `gait={model="res://.../body.scn",
direction={0,0,1}, nominal_speed=1.5, planted_height=0.025,
release_height=0.075}`. Native Cooker samples target-body FK at 60 Hz, finds
low feet moving backwards along the declared direction, and stores the median
stance speed, left/right contact curves and left-foot phase origin in
`locomotion_gait` metadata. Sampling is bounded to 128 bones and 1800 poses; missing
feet, invalid direction/speeds or insufficient stance motion reject the clip.
`direction` uses the assembled model's local coordinates. This metadata is an
offline measurement, not Root Motion or a runtime movement command. Runtime
uses the nominal/measured speed ratio and its own collision velocity to drive
the loop; support anchors remain presentation caches. The project example is
`content/shared/cooker/avatars/actions/pipeline.luau`.

For a humanoid clip, `align_hips_to="move.idle"` rebases each segment's
sampled Hips position to the named semantic's first pelvis position. The
reference must already exist in the base library; the source, all limb tracks
and any vertical change not flattened by `flatten_vertical` remain intact.
This is useful when an in-place source preserves a displaced initial pelvis.
The optional `align_hips_axes="xz"` aligns only the horizontal origin, preserving
the source's authored vertical pose. The default `"xyz"` retains the existing
full-origin alignment. The option requires `align_hips_to` and rejects other axes
or types. Rest/stand clips can therefore share one horizontal origin without
lifting a lying pose to the standing pelvis height. Provenance records the axes.
`validate-character-actions` can explicitly request `include_hips_positions=true`
for finite first/last Hips track values in its cold JSON report. This is diagnostic
data and does not alter the AnimationLibrary or claim visual acceptance.
`rotation_offsets` may turn the Hips bone by up to 180 degrees around Y;
other bone/axis corrections remain limited to 90 degrees. Each offset defaults
to `space="local"` (postmultiply) and may specify `space="parent"`
(premultiply) when a supine or tilted Hips axis makes a local heading change
roll the body. Invalid spaces are rejected. Both settings are authored in the
immutable Luau pipeline and recorded in clip provenance.

A clip can declare `mirror_bones={"LeftShoulder","LeftUpperArm",
"LeftLowerArm","LeftHand"}` to copy those rotation tracks onto their opposite
humanoid side, reflecting the rotation across local X. The array is bounded to
16 unique Left/Right bone names; both source and opposite rotation tracks must
exist. All sources are read from a snapshot so swaps are order independent.
Rest translations, scale and unlisted rotations stay unchanged. Mirroring
precedes `rotation_offsets`, and the selected bones enter composition and
coverage provenance. This is an offline FK operation, not a runtime IK solver.

A composed clip can declare `socket_motion={model="res://.../body.scn",
socket="carry",bone="Chest",upright=true,
offsets={{phase=0,position={0,.30,.24}}}}`. Cooker samples the final target-body
FK at 60 Hz and stores a typed two-track Animation in `socket_motion` metadata.
The position is the average LeftHand/RightHand position plus the declared offset
in skeleton coordinates, transformed into the reference bone's local space.
`upright=true` counter-rotates the reference bone; it does not edit any bone track.
`left` and `right` may name other hand bones. The offset curve allows up to eight
strictly increasing phases from 0 to 1, smoothstep interpolation, and offsets of
at most two metres. A single phase-zero offset is constant. Model FK is bounded
to 128 bones and 1800 samples, and resulting local positions to three metres.
The runtime samples this private presentation channel on the existing socket;
it is not an IK constraint, ECS gameplay transform, or executable property track.
JSON provenance includes its configuration and sample count, not Resource objects.
Segment `blend` accepts 0..2 seconds and is clamped to the segment duration;
it blends from the preceding segment's final pose without adding clip time.

A segment may declare `hold_phase=0.5` to sample one finite source pose while
`begin`, `end` and `speed` continue to determine its duration. The phase must
be numeric and within 0..1. This creates a body-pose basis for authored transfers
without replaying incompatible limb motion from the source.

`limb_paths={model="res://.../body.scn",anchor="Hips",max_target_error=.03,
limbs={{upper="LeftUpperArm",lower="LeftLowerArm",["end"]="LeftHand",
pole={.65,-.15,-.18},keys={{phase=0,position={.36,.5,.32}},
{phase=.5,position={.12,.5,.32}},{phase=1,position={.36,.5,.32}}}}}}`
bakes up to four independent two-bone chains into ordinary FK rotation keys at
60 Hz. Target positions are offsets from the sampled anchor's origin in skeleton
coordinates; the pole is a bend-direction vector in the same coordinates. Linear
interpolation permits a constant support sweep. Each path contains 2..8 strictly
increasing phases spanning 0..1; a loop requires matching endpoint positions.
The host validates direct parent chains, unique writable bones, finite values,
uniform positive chain scale, 128 bones and 1800 samples. It preserves all rest
translations, scales and sampled end-bone global orientation. Unreachable targets
are bounded by `max_target_error` (0..0.15 m); larger errors fail the cook rather
than stretching a limb. Provenance records sample count, clamped sample count and
measured maximum error. This is an offline solve, and adds no runtime IK. Paths
precede socket motion and gait measurement so both sample the final baked pose.
The resource checks do not establish visual contact quality; inspect packaged Veya.

`validate-character-actions` can additionally take `compare_base` and an
`expected_changed` ID array. This cold check compares animation duration, loop,
step, tracks, interpolation and every key against the prior library, bounded to
256 semantics and four million keys. The report lists `changed_ids`,
`retained_ids`, `compared_keys` and the base SHA-256. Unexpected differences or
missing IDs fail validation. This checks motion data, not visual contact quality.
