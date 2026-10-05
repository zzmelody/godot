# TinyUSDZ / LightUSD

Upstream: https://github.com/lighttransport/LightUSD

Source revision: `46c2e36b1d488571e666a2ee9a6619cec6ea064b` (release branch).

Acquired as the upstream release source archive, SHA-256:
`9633b5da34606eb4c2f14f0e5063d1023a167db3675583e3700e290b81d19ce2`.

The immutable upstream `src/` and license are vendored for the Cooker-only
static USD reader. The explicit sources in `cooker/SCsub` exclude bindings,
PXR compatibility, sample renderers and external image/audio implementations.
The upstream Apache 2.0 license and each bundled source's own notices apply.
This library is absent from editor/template/client targets.

Veya patch: namespace the legacy texture helper declarations to avoid the
Godot `Texture` class name; no USD reader semantics are changed.
