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
- [7. Shader modules](#7-shader-modules)
- [8. End-to-end data flow](#8-end-to-end-data-flow)

## 1. Overview

snr is a headless renderer built from three layers that do not overlap:

- A **CPU scene layer** that imports glTF into a plain data structure and derives everything the
  renderer needs (world-space lights, bounds). It never touches the GPU, so it can be inspected in
  isolation.
- A **GPU layer** that uploads that structure once as an immutable snapshot and binds it to the
  shaders.
- A **render layer** that dispatches the Slang path tracer in batches, accumulates into a film
  buffer, and reads the result back for display and file output.

The layers communicate through exactly one shared definition file,
[scene_io.h.slang](../shaders/scene_io.h.slang), which both C++ and Slang include. That file is the
single source of truth for every struct that crosses the host/device boundary.

## 2. Module map

```
src/
  pathtracer.cpp   Command-line front end (owns main). Parses options, loads the model,
                   calls render().
  gpu/             DeviceContext (Vulkan device, queue, validation, shader search path) and
                   ShaderLibrary (Slang module load + compute pipeline cache), plus the
                   makeBuffer/upload helpers and checkRhi.
  scene/           Scene         CPU-side geometry/material/light/instance data
                   GltfImporter  glTF -> Scene
                   GpuScene      Scene -> GPU buffers, textures and acceleration structures
                   scene_json    Scene -> JSON for --inspect
  render/          RenderSettings, RenderResult, Film (accumulation + display buffers),
                   RenderSession (progressive renderer), render() one-shot entry.
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

## 6. Rendering algorithm

### 6.1 Frame execution

`render()` ([renderer.cpp](../src/render/renderer.cpp)) creates the device, loads
`pathtracer.slang`, opens a `RenderSession`, and then:

1. `RenderSession::renderTo(spp, batchSize)` — the progressive accumulation loop.
2. `RenderSession::readback(display)` — tone mapping + buffer copy to host.
3. `saveImages()` — writes the tone-mapped PNG and the linear HDR image.

`renderTo` dispatches `traceMain` in batches. Each batch packs `Params` with
`image.z = samples accumulated so far` and `image.w = batch size`; the shader adds its new samples to
the film buffer in place, so a session accumulates toward a target and can be extended without
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

## 7. Shader modules

| File | Role |
| --- | --- |
| [pathtracer.slang](../shaders/pathtracer.slang) | Entry points `traceMain` and `tonemapMain`; the path loop and the ray cone |
| [scene.slang](../shaders/scene.slang) | BVH traversal, hit resolution, attribute gathering, texture sampling, material lookup, normal mapping, light selection and PDF |
| [bsdf.slang](../shaders/bsdf.slang) | Metallic-roughness BSDF evaluation and sampling |
| [sampling.slang](../shaders/sampling.slang) | RNG, cosine-hemisphere sampling, power heuristic, ray offsetting |
| [scene_io.h.slang](../shaders/scene_io.h.slang) | Structs shared between host and device |

`ShaderLibrary` loads `pathtracer.slang` once; each `computePipeline(entryPoint)` composes the module
with an entry point, links it, and caches the resulting compute pipeline.

## 8. End-to-end data flow

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
                  ▼  RenderSession::renderTo (batched)
              traceMain ──► film (float4 per pixel, accumulative)
                  │
                  ▼  RenderSession::readback
              tonemapMain ──► display
                  │
                  ▼  Film::readback + saveImages
              PNG (tone mapped)  +  HDR (linear)
```
