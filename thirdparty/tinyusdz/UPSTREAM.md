# TinyUSDZ / LightUSD

Upstream: https://github.com/lighttransport/LightUSD

Source revision: `46c2e36b1d488571e666a2ee9a6619cec6ea064b` (release branch).

Acquired as the upstream release source archive, SHA-256:
`9633b5da34606eb4c2f14f0e5063d1023a167db3675583e3700e290b81d19ce2`.

The upstream `src/` and license are vendored for the Cooker-only
static USD reader. The explicit sources in `cooker/SCsub` exclude bindings,
PXR compatibility, sample renderers and external image/audio implementations.
The upstream Apache 2.0 license and each bundled source's own notices apply.
This library is absent from editor/template/client targets.

Veya patches:
- Namespace legacy texture helper declarations to avoid Godot's `Texture` name.
- Retain locally authored children absent from the weaker reference layer.
- Reconstruct PointInstancer prims during Layer-to-Stage composition rather
  than silently dropping the instancer and its prototypes.
- Resolve parents before children against the composed destination layer, so
  internal prototype references can find geometry supplied by a parent layer;
  remap a copy rather than mutating shared reference targets.
