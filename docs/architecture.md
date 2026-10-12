# snr Architecture

How a glTF file becomes a path-traced image: the scene data model, how that model crosses to the
GPU, and the rendering algorithm that consumes it. This document describes the current
implementation; it is not a roadmap.

- [1. Overview](#1-overview)
- [2. Module map](#2-module-map)
- [3. Scene data model and flow](#3-scene-data-model-and-flow)
- [4. Host/device contract](#4-hostdevice-contract)
- [5. GPU upload and binding](#5-gpu-upload-and-binding)
- [6. Rendering algorithm](#6-rendering-algorithm)
- [7. Forward raster preview](#7-forward-raster-preview)
- [8. Shader modules](#8-shader-modules)
- [9. End-to-end data flow](#9-end-to-end-data-flow)

## 1. Overview

snr is a headless renderer built from three layers that do not overlap:

- A **CPU scene layer** that imports glTF into a plain data structure and derives everything the
  renderer needs (world-space lights, bounds). It never touches the GPU, so it can be inspected in
  isolation.
- A **GPU layer** that uploads that structure once as an immutable snapshot and binds it to the
  shaders.
- A **render layer** that runs one of two renderers — a path tracer or a forward raster preview —
  accumulates into a film buffer, and reads the result back for display and file output.

The two renderers share the scene, camera, film, tonemap and output code; only the shading step
differs. The path tracer is described in §6, the raster preview in §7.

The layers communicate through exactly one shared definition file,
[scene_io.h.slang](../shaders/scene_io.h.slang), which both C++ and Slang include. That file is the
single source of truth for every struct that crosses the host/device boundary.

## 2. Module map

```
src/
  main.cpp         Command-line front end for the snr executable (owns main). Parses
                   options, loads the model, calls render().
  gpu/             DeviceContext (Vulkan device, queue, validation, shader search path) and
                   ShaderLibrary (Slang module load + compute pipeline cache), plus the
                   makeBuffer/upload helpers and checkRhi.
  scene/           Scene         CPU-side geometry/material/light/instance data
                   GltfImporter  glTF -> Scene
                   GpuScene      Scene -> GPU buffers, textures and acceleration structures
                   scene_json    Scene -> JSON for --inspect
  render/          RenderSettings, RenderResult, Film (accumulation + display buffers),
                   Session (shared scene snapshot, film, settings, tone-map/readback) with the
                   subclasses PathSession (progressive path tracer) and RasterSession (forward
                   raster preview), dispatch helpers shared by both, render() one-shot entry.
  app/             Window (GLFW window + slang-rhi swapchain surface, resize handling and
                   input polling) for the interactive preview.
  io/              PNG and HDR image output (stb_image_write).
  util/            CommandLine (declarative option registry that owns both parsing and usage)
                   and a minimal JSON writer.
shaders/           Slang sources compiled at runtime by ShaderLibrary.
cmake/             dependency wiring (Slang from the Vulkan SDK or a source build).
```

## 3. Scene data model and flow

### 3.1 Import pipeline

`Scene::loadGltf()` delegates to `GltfImporter::load()`, which runs:

```
loadFile          tinygltf parses the .gltf/.glb into the importer's own model
import            checks the used extensions, selects the glTF scene, then runs:
                    importMaterials   glTF materials -> GltfMaterial records; images ->
                                      TextureData; sampler states -> TextureSampler (deduplicated)
                    importGeometry    walks the node hierarchy: visitNode -> emitMesh ->
                                      buildPrimitive, appending vertices/indices and registering
                                      primitives + instances
finalize          Scene::buildLights() + Scene::computeBounds()
frameCamera       derives eye/target/fov from the bounds
```

The importer owns the loaded `tinygltf::Model` for the lifetime of the `Scene`, so the scene can be
re-derived without re-reading the file (see `Scene::parse`).

### 3.2 The `Scene` struct

`Scene` ([scene.h](../src/scene/scene.h)) is the entire CPU-side model. Geometry is stored as
**typed arrays in structure-of-arrays form**, not as per-triangle records:

| Field | Meaning |
| --- | --- |
| `positions`, `normals` | One `vec3` per vertex. `normals` is zero when the source had no usable normal. |
| `baseColorUvs`, `metallicRoughnessUvs`, `normalUvs`, `occlusionUvs`, `emissiveUvs` | One `vec2` per vertex, one array per material texture slot because every slot may use a different UV set. |
| `indices` | One `uvec3` per triangle. The ids are **local to the owning primitive's vertex window**. |
| `primitives` | `Primitive` records: one triangle range with a single material. |
| `instances` | `Instance` records: a transform plus the primitive it places. |
| `materials`, `lights` | `GltfMaterial` and world-space `Light` records. |
| `triangleLights`, `lightCdf` | Per-triangle light remap and the light power distribution; see §6.5. |
| `textures`, `samplers` | `TextureData` (full mip chain, RGBA8) and `TextureSampler` states. |
| `eye`, `target`, `up`, `fov`, `environment`, `lower`, `upper` | Camera and derived extents. |
| `errors`, `warnings`, `currentScene` | Diagnostics and provenance. |

**Why typed arrays instead of a `Triangle` struct.** Each attribute is a separate buffer that the
shader indexes directly. A `float3` element is 12 bytes and a `float2` is 8 — the tight glTF sizes —
because nothing forces them into a struct whose member would need `float4` padding to be
unambiguous. See §4.

**Per-primitive vertex window.** `buildPrimitive` appends each primitive's referenced vertices as one
contiguous run and rewrites the indices relative to that run. Within a primitive a shared vertex is
stored once (the run is compacted to only the vertices actually referenced), so `positions.size()` is
far below `3 * triangleCount` for indexed meshes. `Primitive::vertexBase`/`vertexCount` record the
window; `Primitive::firstTriangle` is an ordinal into `indices`.

The window is not just bookkeeping — it is required. The acceleration-structure backend derives a
primitive's triangle count from its vertex and index counts (see §5.3), so a primitive must not
reference vertices outside its own triangles.

**Instances and lights.** `addInstance` stores the transform as three explicit rows plus three rows
of the inverse-transpose (for normals), spelled out so host and Slang agree on layout without
relying on either side's matrix convention. `finalize` walks the instances: an instance whose
material emits gets a run of world-space `Light` triangles, but only for the triangles its emissive
texture actually lights up — a black texel is left out so it does not dilute light sampling. Each
instance also gets a run in `triangleLights`, one entry per triangle, holding the light index or `-1`;
`Instance::lightBase` is the start of that run, so a hit's light is
`triangleLights[lightBase + localTriangle]`.

**Texture and sampler dedup.** `importMaterials` registers one `TextureData` per image (with its full
mip chain) and one `TextureSampler` per distinct wrap/filter state. Materials name both by index, so
the shader's bindless arrays are filled in exactly the order the importer registered them.

### 3.3 Lifecycle and ownership

- The `Scene` is plain data; `finalize()` derives lights and bounds and is called once after all
  geometry and instances are in place.
- `--inspect` prints the scene as JSON via `sceneJson()` **before** any `DeviceContext` exists, so
  scene derivation can be checked without a GPU.
- `GpuScene` takes a `const Scene&` and copies nothing back: the CPU data is not retained after
  upload.

## 4. Host/device contract

[scene_io.h.slang](../shaders/scene_io.h.slang) is included by both the C++ and the Slang side, so
the two definitions cannot drift and each size is asserted in one place (Slang has no `sizeof`
type operand, so the assertions are host-only; the shared definition is what keeps layouts equal).

| Struct | Size | Role |
| --- | --- | --- |
| `Primitive` | 20 B | `firstTriangle`, `triangleCount`, `material`, `vertexBase`, `vertexCount` |
| `Instance` | 112 B | Object-to-world rows, normal rows, `primitive`, `material`, `lightBase`, `flags` |
| `Light` | 96 B | World-space `p0/p1/p2`, an info vector whose `.x` is the material index, and the emitter's UVs |
| `GltfMaterial` | 112 B | PBR factors, flags, texture/sampler indices for all five slots, and the normal/occlusion scales |
| `Params` | 112 B | Per-dispatch constants (image, integrator, camera basis, environment) |
| `RasterParams` | 112 B | Raster constants: view-projection rows, eye + `tan(fov/2)`, environment + eye-to-target distance, light count + debug view |

Not shared are the vertex attributes. Positions, normals, UVs and indices are each bound as their
own typed array (`StructuredBuffer<float3>`, `<float2>`, `<uint3>`), which is what lets the elements
keep their tight glTF sizes instead of being padded to `float4` — the padding was only ever needed to
make a `float3` struct member unambiguous.

`SNR_MAX_TEXTURES` (1024) and `SNR_MAX_SAMPLERS` (32) live here too. The host sizes its bindless
descriptor pool from these constants, so the shader's handle-array bounds and the pool capacity
cannot disagree.

## 5. GPU upload and binding

`GpuScene` ([gpu_scene.cpp](../src/scene/gpu_scene.cpp)) is an immutable GPU snapshot built in one
constructor.

### 5.1 Geometry buffers

`positions` and `indices` are uploaded once with
`BufferUsage::ShaderResource | BufferUsage::AccelerationStructureBuildInput`. The **same two buffers
are read by the acceleration structure build and by the shader**, so no second copy of the geometry
is uploaded and there is no padded shadow copy. `normals`, the five UV arrays, the primitives,
instances, materials and lights go up as plain `ShaderResource` buffers, as do the `triangleLights`
remap and the `lightCdf` distribution.

### 5.2 Textures and samplers (bindless)

Each `TextureData` becomes one `ITexture` with its full mip chain (RGBA8, sRGB variant when the
image is a colour texture), an `ITextureView`, and a descriptor handle pulled from the view. Each
`TextureSampler` becomes one `ISampler` with a matching handle. The handles are written into the
shader's bindless `images[]`/`samplers[]` arrays at the index the material names. The `ITexture`,
`ITextureView` and `ISampler` objects are retained for the renderer's lifetime, because the
descriptor handle is owned by them.

### 5.3 Acceleration structures

One **BLAS per primitive**, built from indexed triangles over the primitive's own vertex window:
vertex format `RGB32Float`, stride 12, vertex data offset to `vertexBase * 12`, index data offset to
`firstTriangle * 3 * 4`, `vertexCount = primitive.vertexCount`, `indexCount = triangleCount * 3`.
Every BLAS build shares one encoder and one submit; each keeps its own scratch buffer.

One **TLAS** references every instance: the instance descriptors carry the transform, an `instanceID`
used as the shader's hit lookup, the instance mask, and the corresponding BLAS handle.

> The slang-rhi Vulkan backend computes a geometry's primitive count as
> `max(vertexCount, indexCount) / 3` and `maxVertex = vertexCount - 1`. A primitive must therefore
> satisfy `vertexCount <= 3 * triangleCount`, which is exactly what the per-primitive vertex window
> guarantees. Describing a primitive with the global vertex count would make the backend read the
> index buffer out of range.

### 5.4 Binding

`GpuScene::bind()` distinguishes the two binding models:

- **Buffers** (`sceneBVH`, `positions`, `normals`, `baseColorUvs`, `metallicRoughnessUvs`,
  `normalUvs`, `occlusionUvs`, `emissiveUvs`, `indices`, `primitives`, `instances`, `materials`,
  `lights`, `triangleLights`, `lightCdf`) are bound by **name** into descriptor set 0.
- **Textures and samplers** are written into their **bindless slots** by `setDescriptorHandle`, one
  per registered image/sampler.

`GpuScene::bindRaster()` binds the same buffers **except** `triangleLights`, which the raster program
never declares. `sceneBVH` *is* bound: the fragment shader's shadow rays are inline queries against the
same top-level structure the path tracer uses. `GpuScene` also exposes one `InstanceDraw` per instance
for the draw loop in §7 — the primitive and its triangle count (which together give the draw's vertex
count), whether the instance's material is glTF BLEND (which picks the draw's group), and the
instance's world origin (which orders the transparent group).

## 6. Rendering algorithm

### 6.1 Frame execution

`render()` ([renderer.cpp](../src/render/renderer.cpp)) creates the device, loads the shader module
for the selected mode (`RenderingMode::PathTracing` or `RenderingMode::Raster`, set by `--raster`),
opens a `PathSession` or a `RasterSession`, and then runs the same three steps for either:

1. Render — `render()`. The path tracer accumulates its sample batches; the raster preview records
   its one pass.
2. Readback — `readback(display)` — tone mapping + buffer copy to host.
3. `saveImages()` — writes the tone-mapped PNG and the linear HDR image.

Both sessions derive from `Session` ([session.h](../src/render/session.h)), which owns the scene
snapshot, the settings, the film and the tone-map/readback step, and declares the pure virtual
`render()`. So the two renderers share everything except the pipelines and the draw itself — including
the sample-count bookkeeping, `resize`/`setCamera`, and the `samples()`/`reset()` surface a future
interactive loop needs. `PathSession` adds the integrator/environment/scene setters and the
accumulation loop; `RasterSession` adds the offscreen targets and the draw loop of §7. Output,
exposure and the image pipeline are therefore literally the same code for both; §7 covers the one
place they diverge.

`PathSession::render()` dispatches `traceMain` in batches until the film reaches
`RenderSettings::samplesPerPixel`, in steps of `RenderSettings::batchSize`. Each batch packs `Params`
with `image.z = samples accumulated so far` and `image.w = batch size`; the shader adds its new samples
to the film buffer in place, so a session accumulates toward a target and can be extended without
re-rendering earlier samples.

`Params` packing ([session.cpp](../src/render/session.cpp)):

| Field | Contents |
| --- | --- |
| `image` | width, height, accumulated sample base, batch sample count |
| `integrator` | max bounces, RNG seed, light count, NEE enabled |
| `eye` | camera position, and `tan(fov/2)` in `.w` |
| `forward` | normalized view direction, aspect ratio in `.w` |
| `right`, `up` | orthonormal camera basis |
| `environment` | constant environment radiance, exposure in `.w` |

### 6.2 Path tracing (`traceMain` → `tracePath`)

Per sample, `traceMain` jitters the pixel, builds the primary ray from the camera basis, and calls
`tracePath`. `tracePath` is a standard unidirectional path tracer with next-event estimation:

1. Trace the ray against the BVH. On a miss, add `throughput * environment` and stop.
2. Resolve the hit: instance → primitive → triangle (attributes gathered from the shared arrays) →
   surface (`position`, geometric normal, shading normal, material).
3. Add emitted radiance. If the previous bounce used NEE and this hit is an emitter, weight the
   emission by `powerHeuristic(previousPdf, lightPdf)` so emission is not double counted.
4. **NEE**: sample one light in proportion to its power, pick a point on it, trace a shadow ray
   (`anyHit`), and on a clear path add `throughput * f * Le * cos / pdf_light`, weighted by
   `powerHeuristic(pdf_light, pdf_bsdf)`. The light's emission is read at the sampled point, so a
   textured emitter is estimated correctly.
5. **BSDF sample**: choose and sample a lobe, multiply throughput by `f * cos / pdf`. A zero weight
   or a sample below the geometric surface terminates the path.
6. **Russian roulette** after the third bounce, using the throughput's largest channel as survival
   probability.
7. Re-seed the ray-cone width from the footprint at the hit and offset the origin before the next
   segment.

### 6.3 Materials and BSDF

`materialAt` produces a `SurfaceMaterial` (base color, emission, metallic, roughness, occlusion) by
multiplying the glTF factors with the matching textures. The model is metallic-roughness:

- A per-hit probability splits between a diffuse lobe and a specular lobe.
- Specular uses a GGX distribution with VNDF sampling and height-correlated Smith visibility, and
  Schlick Fresnel.
- Sub-milliperceptual roughness is treated as a **delta** mirror lobe, which also marks the sample as
  delta so NEE is not applied after it.
- A material without the PBR flag falls back to diffuse-only (`material.flags.y`).

**Emission and occlusion.** Emission is `emissiveFactor * emissiveTexture`, and the same expression
is what `lightEmission` returns for a sampled light point, so a directly hit emitter and a sampled
one agree for MIS. The occlusion map's red channel, lerped by `occlusionStrength`, scales only the
diffuse lobe; the specular lobe is left alone.

**Normal mapping.** A ray-traced hit has no screen-space derivatives, so the tangent frame comes from
Mikkelsen's derivative-free construction: `dP/du` and `dP/dv` are taken from the triangle's
world-space edges and its normal-map UVs, orthogonalised against the interpolated shading normal, and
the sampled tangent-space normal perturbs that normal. Degenerate UVs leave the interpolated normal
unchanged, and a perturbed normal that tilts past the surface plane is flipped back toward the
geometry.

**Texture LOD.** Ray tracing has no screen-space derivatives, so `Sample()`'s implicit LOD cannot
pick a mip. Instead the ray-cone footprint at the hit is converted to texture space with the
triangle's texel density (UV area per world area) and passed to `SampleGrad` as an isotropic
gradient. The footprint widens on grazing surfaces, which is what keeps those lookups from aliasing.

### 6.4 Traversal and hit resolution

`traceScene` runs an inline `RayQuery` (skipping procedural primitives). Each candidate hit is
committed only if the material is double sided or the triangle faces the ray, so back faces are
culled for single-sided materials. The committed hit carries `t`, `instanceID`,
`primitiveIndex` and barycentrics, which index straight into `instances`, `primitives` and the
vertex arrays. Mirrored instances still face correctly because world-space normals come from the
transformed edges.

### 6.5 Light sampling

Emissive geometry is flattened into world-space `Light` triangles during `finalize`, each carrying
its material index and its emissive UVs so a textured emitter can be read at the sampled point. Two
things keep the light list small and well balanced:

- **Pruning.** A triangle whose emissive texture is black at its centroid is not added to the list.
  It still emits when a BSDF sample hits it (the emission term is unchanged); it is simply not a NEE
  target, so a mostly-black emissive map does not bury the real lights.
- **Power weighting.** Each light is weighted by its emitted radiance times its world-space area, and
  `lightCdf` is the prefix sum. `sampleLightIndex` draws a uniform value and reads the light off the
  interval it lands in, so a bright light is sampled more often than a dim one. `lightPdf` scales the
  uniform-on-the-triangle density by that selection probability, and `Instance::triangleLights` maps
  a hit back to its light so a directly hit emitter is weighted by exactly the same density.

### 6.6 Accumulation and tone mapping

The film is a `float4` per pixel holding `(radiance sum, sample count)`. `tonemapMain` divides by the
count, applies exposure (`exp2`), tone maps with Reinhard (`x / (x + 1)`), converts linear to sRGB,
and writes the display buffer. `Film::readback()` copies both the accumulation and display buffers to
the host; `saveImages()` writes the display buffer as PNG and a full-precision linear image as HDR.

### 6.7 Sampling utilities

`hash32` builds the per-sample RNG state from pixel, sample index and seed; `randomFloat` is a
small counter-based PRNG. `offsetRay` nudges ray origins off the surface to avoid self-intersection.

## 7. Forward raster preview

`RasterSession` ([rasterizer.cpp](../src/render/rasterizer.cpp)) is the other `Session` subclass: the
scene snapshot, film, settings, tone-map and readback all come from the base, so it only adds the
graphics pipeline, the offscreen targets and the draw loop. It is selected by `--raster`. It exists so
an interactive preview does not pay for a full path trace. It is **forward and single-pass** — shading
happens in the fragment shader, with no G-buffer and no second pass.

### 7.1 Pass and draw loop

`render()` records one render pass with two colour targets — an RGBA32Float colour target (cleared to
the scene's environment radiance, so pixels the geometry misses match the path tracer's miss radiance)
and an R32Uint object-id target — plus a D32Float depth target. The depth test is **reversed-Z**
(`clip.z` is the constant near plane and `clip.w` the view depth, so stored depth is `near / viewZ`)
with `ComparisonFunc::Greater`, which keeps float precision uniform and needs no far plane. Culling is
off, matching the path tracer's lack of culling.

Two pipelines share the `rasterVS`/`rasterFS` pair.
`ShaderLibrary::renderPipeline` is keyed by the entry-point pair **and** a variant name, so asking for
both returns two distinct pipelines instead of the first one twice:

- the **opaque** pipeline draws every instance whose material is not BLEND, with depth writes on and
  no blending;
- the **blend** pipeline draws the BLEND instances after them, with source-over blending
  (`SrcAlpha`/`InvSrcAlpha`) and depth writes off, and with the alpha channel's blend factors pinned
  to `Zero`/`One` so the colour target's alpha — the film's sample counter, §6.6 — keeps the value the
  clear left instead of accumulating coverage.

One draw is issued per instance — `DrawArguments{vertexCount = triangleCount * 3, instanceCount = 1,
startInstanceLocation = instanceIndex}` — with no vertex or index buffer bound. The instance index
reaches the vertex shader through `SV_StartInstanceLocation` (`SV_InstanceID` reports
`instanceID - baseInstance` under D3D semantics and carries nothing usable here).

Both groups are recorded into the one pass, so the transparent group tests against the depth the
opaque group wrote with no second pass and no depth copy. Shading BLEND later than everything else is
what makes sorting enough: the pass binds the opaque pipeline, draws, and then — only if the scene has
a BLEND instance — binds the blend pipeline and draws those, ordered back to front by squared distance
from the camera eye to each instance's world origin. Binding a pipeline replaces the root object, so
the constants and the scene arrays are bound again for the second group; the viewport is set again
alongside them. Sorting whole instances rather than triangles is the approximation a preview can
afford, and `std::stable_sort` keeps scene order for equal distances.

### 7.2 Vertex pulling

`rasterVS` reads `SV_VertexID` and derives `triangle = vertexId / 3`, `corner = vertexId % 3`, then
addresses `indices[primitive.firstTriangle + triangle] + primitive.vertexBase`. Because each
primitive owns a contiguous vertex window, this resolves a corner with no index buffer, no
vertex-buffer usage change, and no second copy of position data — the §4 hard constraint holds.
The vertex is transformed by `RasterParams.viewProjRows` and handed on as `SV_Position`, with the
world position and the instance index passed through as varyings. The projection rows reproduce the
path tracer's ray basis, so the two renderers frame a model identically.

### 7.3 Fragment shading

`rasterFS` uses `SV_PrimitiveID` and `SV_Barycentrics` to pull the triangle's normal/UV corners and
interpolate them, then calls the **shared** `surfaceAt` from
[material_eval.h.slang](../shaders/material_eval.h.slang) — the same material evaluation the path
tracer uses, so base color, metallic-roughness, normal mapping and occlusion cannot diverge between
the two. The one difference is the texture fetch: `material_eval.h.slang` is compiled with
`RASTER_PIPELINE` defined, which selects implicit-LOD `Sample()` (a fragment shader has valid
derivatives) instead of the ray tracer's explicit-gradient `SampleGrad`.

Shading combines three terms:

- emissive term from the material;
- an environment-radiance ambient term scaled by occlusion (diffuse for dielectrics, tinted by base
  color for metals);
- a short deterministic loop over the existing `lights`/`lightCdf`, point-sampled at each light's
  centroid with the area-to-solid-angle pdf — the structural equivalent of the path tracer's NEE —
  where each sample traces an occlusion ray through `occluded()` in
  [material_eval.h.slang](../shaders/material_eval.h.slang) and is skipped when the segment to the
  emitter is blocked.

The occlusion test is an inline `RayQuery` in the fragment shader, traced against the same top-level
acceleration structure the path tracer uses, which the raster root signature binds alongside the
geometry buffers. Both endpoints are biased with `offsetRay` exactly as the NEE ray is, so neither the
receiver nor the emitter self-intersects. The environment term stays unshadowed: a constant
environment has no direction to trace toward.

`rasterFS` also applies the glTF `alphaMode` the importer stored in `GltfMaterial::flags.z`. A MASK
fragment whose interpolated base-color alpha is below `alphaCutoff` is discarded, and its survivors
are opaque; a BLEND fragment passes its coverage on as the fragment's opacity for the blend pipeline
(§7.1) to blend with; OPAQUE ignores coverage. Only the blended pass may carry coverage into the
colour target's alpha, because that channel is the film's sample counter (§6.6) — which is exactly why
the blend pipeline pins its alpha blend factors to `Zero`/`One`.

Every fragment also writes `SV_Target1`, the one-based instance index, so the object-id target records
which instance is under each pixel for picking or segmentation; unwritten pixels keep the zero the
clear left, which is outside the instance range. The two outputs are declared as one struct with
explicit `SV_Target0`/`SV_Target1` members: a return value plus an `out` parameter gets its locations
assigned in the opposite order, which silently writes the id into the colour target.

The shaded colour can be replaced with one channel of the pass, selected by `RasterParams::counts.y`
(one of the `rasterDebug*` constants) and switched at runtime by the window's number keys 1-5 through
`RasterSession::setDebugView()`. The views are a preview aid the path tracer cannot offer, since it
has no such channels, so they are reachable only from the interactive preview: the linear view depth
normalised by the eye-to-target distance, a stable hue per object id, the base color, or the shading
normal mapped to `[0, 1]`. The colour target is then cleared to black instead of the environment, so
uncovered pixels read as "no data" rather than as the backdrop. Debug values are read as data, not as
radiance, and still pass through the shared tone map on their way to the swapchain.

So a preview keeps the direct lighting and its shadows but lacks indirect light; that is the
documented difference from the path tracer.

### 7.4 Into the film and out

The pass leaves linear radiance in the colour target. `copyTextureToBuffer` copies it into
`Film::accumulation()` as a single sample, after which the existing `tonemapMain` +
`Film::readback()` + `saveImages()` path runs unchanged. `tonemap.h.slang` is included by both
`pathtracer.slang` and `rasterizer.slang`, so exposure and the display transform are literally the
same code in both renderers. (This is why `Film`'s accumulation buffer carries
`BufferUsage::CopyDestination` and the colour target `TextureUsage::CopySource`.)

### 7.5 Windowed presentation

With `--window` the headless loop is replaced by an interactive one, and the colour target becomes
"offscreen or swapchain" rather than a fixed texture. `Window` ([window.cpp](../src/app/window.cpp))
uses GLFW only for the window and input: the swapchain itself is slang-rhi's, created from
`getWindowHandleFromGLFW` so the same code path serves Win32, X11 and Cocoa. `pump()` polls events,
reconfigures the surface on a framebuffer resize and reports the new extent, which `renderWindowed`
mirrors onto the session with `Session::resize`. The same loop drives the debug views: a number key
pressed since the last pump is reported by `numberKeyPressed()`, and `renderWindowed` maps key 1-5 to
a `rasterDebug*` view and hands it to the session, so the switch costs no extra rendering path.

Each frame is `render()` (the §7.1 pass into the film, unchanged) followed by `present()`. The latter
runs `presentVS`/`presentFS` — a fullscreen triangle that reads the film at the pixel named by
`SV_Position` — into the acquired swapchain image, and it creates that pipeline lazily from the
target's format, so the surface, not the session, decides the swapchain format. Both the film write
and the present pass call the same `tonemapCore`, so the on-screen image matches the offscreen one.
If the swapchain format is sRGB the shader writes linear values and lets the hardware encode;
otherwise it encodes with `linearToSrgb` itself. Submitting does not wait, as the queue's own
synchronization orders the frame ahead of the present.

Mouse input is polled by the window and turned into a camera by `applyCameraInput` in
[renderer.cpp](../src/render/renderer.cpp): left drag orbits, right drag pans, the wheel dollies, and
each change goes through `Session::setCamera`, which resets the film and redraws. Headless stays the
default, so `--inspect`, the byte-comparable path-trace regression and `--raster` are untouched.

## 8. Shader modules

| File | Role |
| --- | --- |
| [pathtracer.slang](../shaders/pathtracer.slang) | Entry point `traceMain`; the path loop and the ray cone |
| [rasterizer.slang](../shaders/rasterizer.slang) | Entry points `rasterVS`/`rasterFS` plus `presentVS`/`presentFS` for the window; defines `RASTER_PIPELINE` and declares the scene bindings minus the BVH and light remap |
| [scene.slang](../shaders/scene.slang) | BVH traversal, hit resolution, material lookup, light selection and PDF |
| [material_eval.h.slang](../shaders/material_eval.h.slang) | Texture fetch, corner interpolation, normal mapping and `surfaceAt` material evaluation, as plain functions over the bindless arrays; shared by both pipelines |
| [tonemap.h.slang](../shaders/tonemap.h.slang) | `tonemapCore` exposure + Reinhard and the sRGB encode, plus `tonemapMain`; shared by both pipelines |
| [bsdf.slang](../shaders/bsdf.slang) | Metallic-roughness BSDF evaluation and sampling |
| [sampling.slang](../shaders/sampling.slang) | RNG, cosine-hemisphere sampling, power heuristic, ray offsetting |
| [scene_io.h.slang](../shaders/scene_io.h.slang) | Structs shared between host and device |

`material_eval.h.slang` and `tonemap.h.slang` declare **no bindings of their own**. The including
module declares the bindless image/sampler arrays and the `film`/`display` buffers and passes them in
as function parameters (`Texture2D<float4>.Handle images[]`, `SamplerState.Handle samplers[]`). That
is what lets one header serve both pipelines without either one linking the other's globals — in
particular the raster program never links `sceneBVH`.

`ShaderLibrary` loads a module once; `computePipeline(entryPoint)` and `renderPipeline(vertex,
fragment, desc)` compose it with the requested entry points, link, diagnose, and cache the resulting
pipeline.

## 9. End-to-end data flow

```
 glTF file
     │  tinygltf
     ▼
 GltfImporter ──► Scene ──────────────────────────────► sceneJson (--inspect, no GPU)
                  │  typed vertex arrays, primitives, instances,
                  │  materials, lights, textures, samplers
                  │
                  ▼  GpuScene (one immutable snapshot)
   buffers ──┬─► positions / indices ──► BLAS geometry + shader arrays
             ├─► normals / 5 UV arrays / primitives / instances / materials / lights
             │   / triangleLights / lightCdf  (named bindings)
             └─► TLAS over instances
   textures ─┴─► ITexture / ITextureView ─┐
                                          ├─► bindless images[] / samplers[] (handle slots)
   samplers ───► ISampler ────────────────┘
                  │
                  ├─► PathSession::render (batched)                [--raster off]
                  │      traceMain ──► film (float4 per pixel, accumulative)
                  │
                  └─► RasterSession::render                         [--raster]
                         rasterVS/rasterFS ──► colour + object id ──► copy ──► film
                                               depth
                  │
                  ▼  Session::readback (either subclass)
              tonemapMain ──► display
                  │
                  ▼  Film::readback + saveImages
              PNG (tone mapped)  +  HDR (linear)
```
