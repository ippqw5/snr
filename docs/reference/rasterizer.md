# Rasterizer implementation

Read the architecture's [Raster frame](../architecture.md#raster-frame) first. This reference
covers pipeline state, vertex pulling, fragment shading and the transfer into Film.

[Architecture](../architecture.md) · [Project README](../../README.md)

The preview uses one forward pass with no G-buffer. Its host implementation is
[rasterizer.cpp](../../src/render/rasterizer.cpp), with shader entries `rasterVS`, `rasterOpaqueFS` and `rasterFS` in
[rasterizer.slang](../../shaders/rasterizer.slang).

## Pass and draw loop

`render()` records one render pass with two colour targets — an RGBA32Float colour target (cleared to
the scene's environment radiance, so pixels the geometry misses match the path tracer's miss radiance)
and an R32Uint object-id target — plus a D32Float depth target. The depth test is **reversed-Z**
(`clip.z` is the constant near plane and `clip.w` the view depth, so stored depth is `near / viewZ`)
with `ComparisonFunc::Greater`, which keeps float precision uniform and needs no far plane. Culling is
off, matching the path tracer's lack of culling.

Three pipelines share `rasterVS` and the `shadeRasterPixel` helper.
`ShaderLibrary::renderPipeline` is keyed by the entry-point pair **and** a variant name:

- the **opaque** pipeline uses `rasterOpaqueFS` with `[earlydepthstencil]`, so depth-hidden fragments
  skip material evaluation, transmission traversal and all 32 light samples;
- the **mask** pipeline uses `rasterFS` with normal depth writes, so alpha-discarded fragments leave
  depth untouched. OPAQUE and MASK draws retain their original scene order;
- the **blend** pipeline draws the BLEND instances after them, with source-over blending
  (`SrcAlpha`/`InvSrcAlpha`) and depth writes off, and with the alpha channel's blend factors pinned
  to `Zero`/`One` so the colour target's alpha — the Film's per-pixel sample counter — keeps the value the
  clear left instead of accumulating coverage.

One draw is issued per instance — `DrawArguments{vertexCount = triangleCount * 3, instanceCount = 1,
startInstanceLocation = instanceIndex}` — with no vertex or index buffer bound. The instance index
reaches the vertex shader through `SV_StartInstanceLocation` (`SV_InstanceID` reports
`instanceID - baseInstance` under D3D semantics and carries nothing usable here).

Opaque/MASK and BLEND draws are recorded into the same pass, so the transparent group tests against the depth the
opaque group wrote with no second pass and no depth copy. Shading BLEND later than everything else is
what makes sorting enough: OPAQUE/MASK draws keep scene order, switching pipelines as needed.
BLEND draws follow in back-to-front order by squared camera distance to each instance origin.
Binding a pipeline replaces the root object, so constants, scene arrays and viewport are set
for every pipeline binding. Sorting whole instances rather than triangles is the approximation a preview can
afford, and `std::stable_sort` keeps scene order for equal distances.

## Vertex pulling

`rasterVS` reads `SV_VertexID` and derives `triangle = vertexId / 3`, `corner = vertexId % 3`, then
addresses `indices[primitive.firstTriangle + triangle] + primitive.vertexBase`. Because each
primitive owns a contiguous vertex window, this resolves a corner with no index buffer, no
vertex-buffer usage change, and no second copy of position data — the [shared geometry contract](gpu-resources.md#hostdevice-contract) holds.
The vertex is transformed by `RasterParams.viewProjRows` and handed on as `SV_Position`, with the
world position and the instance index passed through as varyings. The projection rows reproduce the
path tracer's ray basis, so the two renderers frame a model identically.

## Fragment shading

`rasterFS` uses `SV_PrimitiveID` and `SV_Barycentrics` to pull the triangle's normal/UV corners and
interpolate them, then calls the **shared** `surfaceAt` from
[material_eval.h.slang](../../shaders/material_eval.h.slang) — the same material evaluation the path
tracer uses, so base color, metallic-roughness, normal mapping and occlusion cannot diverge between
the two. The one difference is the texture fetch: `material_eval.h.slang` is compiled with
`RASTER_PIPELINE` defined, which selects implicit-LOD `Sample()` (a fragment shader has valid
derivatives) instead of the ray tracer's explicit-gradient `SampleGrad`.

Shading combines three terms:

- emissive term from the material;
- an environment-radiance ambient term scaled by occlusion (diffuse for dielectrics, tinted by base
  color for metals);
- sampled direct lighting.

Raster samples only the triangle prefix of the light CDF for its 32 area-light samples, normalizing
selection probabilities against that prefix's total power. Point lights are evaluated separately,
once each per shaded surface, with inverse-square falloff and one binary shadow ray. This also
applies to secondary surfaces reached by the transmission preview. No shadow-map pass is needed.

The occlusion test is an inline `RayQuery` in the fragment shader, traced against the same top-level
acceleration structure the path tracer uses, which the raster root signature binds alongside the
geometry buffers. Both endpoints are biased with `offsetRay` exactly as the NEE ray is, so neither the
receiver nor the emitter self-intersects. Raster shadows retain binary triangle visibility, including
for transparent materials. The query forces opaque triangle traversal and accepts the first blocker,
avoiding shader candidate handling and a closest-hit search. These flags are limited to raster shadow
queries; transmission and path-tracer queries keep their material-aware traversal.
The environment term stays unshadowed: a constant
environment has no direction to trace toward.

`rasterFS` reads glTF `alphaMode` from `GltfMaterial::flags.z`. MASK discards fragments below
`alphaCutoff`; OPAQUE ignores coverage; BLEND returns opacity for the
[blend pipeline](#pass-and-draw-loop).

Every fragment also writes `SV_Target1`, the one-based instance index, so the object-id target records
which instance is under each pixel for picking or segmentation; unwritten pixels keep the zero the
clear left, which is outside the instance range. The two outputs are declared as one struct with
explicit `SV_Target0`/`SV_Target1` members: a return value plus an `out` parameter gets its locations
assigned in the opposite order, which silently writes the id into the colour target.

`RasterParams::counts.y` selects a `rasterDebug*` view via `RasterSession::setDebugView()`.
Debug data is linear view depth normalized by eye-to-target distance, a stable hue per object ID,
base color, or the shading normal mapped to `[0, 1]`. Debug views clear uncovered pixels to black
instead of environment radiance and still pass through the shared tone map. These channels are
only available in raster mode; controls are listed under [usage](../../README.md#interactive-controls).

## Transfer into Film

`copyTextureToBuffer` transfers the linear color target into Film accumulation as one sample.
This requires `CopySource` on the texture and `CopyDestination` on the accumulation buffer.
Subsequent [window presentation or file export](../architecture.md#presentation-and-file-output) uses the shared output paths.
