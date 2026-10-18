# Raster rendering

Raster provides an immediate preview over the same scene snapshot, material evaluation and
Film contract used by Path tracing. Read the architecture's
[Raster frame](../architecture.md#raster-frame) for its place in the rendering flow.

## Pass and draw loop

The renderer uses a forward pass with color, object-identity and depth targets. It draws instances
over shared scene geometry. Reversed depth supports precision across scene scales, while a common
camera model keeps Raster and Path tracing framing aligned.

Opaque and alpha-masked geometry establish depth before blended surfaces are composited.
Masked surfaces write depth only where coverage is accepted. Blended surfaces use the existing
depth without replacing it, so surfaces behind them can remain visible.

Transparency is ordered by instance distance. This is a preview approximation: sorting whole
instances cannot resolve every triangle overlap or intersection.

## Shared geometry and materials

Vertex pulling reads the uploaded scene arrays directly. Raster and ray traversal therefore share
one geometry representation and primitive-local addressing convention. Material evaluation uses
the same imported factors, textures and normal handling as the path tracer.

The [GPU resource design](gpu-resources.md#hostdevice-contract) describes the shared boundary;
shader input semantics and pipeline state are defined in the renderer sources.

## Fragment shading

Shading combines material emission, an ambient term from constant environment radiance and
sampled direct lighting. Triangle lights are sampled from the shared light distribution. Point
lights are evaluated separately. The environment term remains unshadowed.

Ray queries against the shared TLAS provide binary shadows and a transmission preview, so Raster
also requires ray-query support. Secondary transmitted surfaces receive direct and ambient
shading. Reflection and indirect transport remain limited compared with Path tracing; see
[transmission and volume transport](light-transport.md#transmission-and-volume-transport).

Debug views expose geometry or material data through the same display path. Controls are described
in [Usage](../usage.md#interactive-controls).

## Transfer into Film

The color target contains linear radiance. Copying it into Film makes Raster compatible with the
shared [display and export paths](../architecture.md#presentation-and-file-output).

Coverage opacity and Film's sample count serve different purposes. Alpha blending preserves the
sample count so that a Raster frame remains one sample per pixel when displayed or exported.

## Code entry points

| Source | What to look for |
| --- | --- |
| [rasterizer.cpp](../../src/render/rasterizer.cpp) | Render targets, pipeline variants, draw ordering and Film transfer |
| [rasterizer.slang](../../shaders/rasterizer.slang) | Vertex pulling, shared surface evaluation, lighting and debug views |
| [gpu_scene.cpp](../../src/scene/gpu_scene.cpp) | Per-instance draw metadata and resource bindings |
