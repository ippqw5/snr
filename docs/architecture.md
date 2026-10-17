# Architecture reference

Use this document to look up implementation details. Execution order, ownership, output paths and
reset rules are explained once in the [illustrated rendering flow](rendering-flow.md).
Build, usage and verification commands live in the [project README](../README.md).

- [Code map](#code-map)
- [Scene representation](#scene-representation)
- [Host/device contract](#hostdevice-contract)
- [GPU resource details](#gpu-resource-details)
- [Shading details](#shading-details)
- [Raster implementation](#raster-implementation)
- [Presentation implementation](#presentation-implementation)
- [Transmission and volume transport](#transmission-and-volume-transport)

## Code map

| Implementation | Responsibility |
| --- | --- |
| [main.cpp](../src/main.cpp) | CLI parsing, initial model selection and `--inspect` |
| [gltf_importer.cpp](../src/scene/gltf_importer.cpp), [scene.cpp](../src/scene/scene.cpp) | Import and CPU scene derivation |
| [gpu_scene.cpp](../src/scene/gpu_scene.cpp) | Uploads, descriptors, acceleration structures and draw metadata |
| [session.cpp](../src/render/session.cpp), [film.cpp](../src/render/film.cpp) | Session setters, progressive batches and image storage |
| [dispatch.h](../src/render/dispatch.h), [device.cpp](../src/gpu/device.cpp) | Compute recording, submission and synchronization |
| [shader_library.cpp](../src/gpu/shader_library.cpp) | Runtime module loading, composition, diagnostics and pipeline caching |
| [renderer.cpp](../src/render/renderer.cpp) | Headless orchestration, GUI loop and asynchronous model import |
| [rasterizer.cpp](../src/render/rasterizer.cpp) | Raster targets, pipeline variants and draw loop |
| [window.cpp](../src/app/window.cpp), [preview_ui.cpp](../src/app/preview_ui.cpp) | Swapchain, input, ImGui and detached panels |
| [image_output.cpp](../src/io/image_output.cpp) | PNG / HDR output |
| [pathtracer.slang](../shaders/pathtracer.slang) | `traceMain`, `tracePath` and ray cones |
| [scene.slang](../shaders/scene.slang) | Ray queries, hit resolution and scene resource declarations |
| [material_eval.h.slang](../shaders/material_eval.h.slang) | Shared attribute / texture lookup, material evaluation and light helpers |
| [bsdf.slang](../shaders/bsdf.slang), [medium.slang](../shaders/medium.slang) | BSDF evaluation / sampling and medium state |
| [rasterizer.slang](../shaders/rasterizer.slang) | `rasterVS`, `rasterOpaqueFS`, `rasterFS` and preview shading |
| [present.slang](../shaders/present.slang), [tonemap.h.slang](../shaders/tonemap.h.slang) | Window presentation and shared tone-mapping functions |
| [sampling.slang](../shaders/sampling.slang) | RNG, sampling distributions, MIS heuristic and ray offsets |
| [scene_io.h.slang](../shaders/scene_io.h.slang) | Shared host/device records |
| [Dependencies.cmake](../cmake/Dependencies.cmake) | Local dependency integration |

ShaderLibrary caches modules and pipelines. Graphics pipelines are keyed by entry-point pair and
variant name, so opaque, masked and blended raster states can share entry points.

## Scene representation

`Scene` ([scene.h](../src/scene/scene.h)) is the entire CPU-side model. Geometry is stored as
**typed arrays in structure-of-arrays form**, not as per-triangle records:

| Field | Meaning |
| --- | --- |
| `positions`, `normals` | One `vec3` per vertex. `normals` is zero when the source had no usable normal. |
| `baseColorUvs`, `metallicRoughnessUvs`, `normalUvs`, `occlusionUvs`, `emissiveUvs`, `transmissionUvs` | One `vec2` per vertex, one array per material texture slot because every slot may use a different UV set. |
| `indices` | One `uvec3` per triangle. The ids are **local to the owning primitive's vertex window**. |
| `primitives` | `Primitive` records: one triangle range with a single material. |
| `instances` | `Instance` records: a transform plus the primitive it places. |
| `materials`, `lights` | `GltfMaterial` and world-space `Light` records. |
| `triangleLights`, `lightCdf` | Per-triangle light remap and the light power distribution; see [light sampling](#light-sampling). |
| `textures`, `samplers` | `TextureData` (full mip chain, RGBA8) and `TextureSampler` states. |
| `eye`, `target`, `up`, `fov`, `environment`, `lower`, `upper` | Camera and derived extents. |
| `errors`, `warnings`, `currentScene` | Diagnostics and provenance. |

**Per-primitive vertex window.** `buildPrimitive` appends each primitive's referenced vertices as one
contiguous run and rewrites the indices relative to that run. Within a primitive a shared vertex is
stored once (the run is compacted to only the vertices actually referenced), so `positions.size()` is
far below `3 * triangleCount` for indexed meshes. `Primitive::vertexBase`/`vertexCount` record the
window; `Primitive::firstTriangle` is an ordinal into `indices`.

The window is not just bookkeeping — it is required. The acceleration-structure backend derives a
primitive's triangle count from its vertex and index counts
(see [AS constraints](#acceleration-structures)), so a primitive must not reference vertices outside
its own triangles.

**Transforms.** `addInstance` stores three explicit object-to-world rows and three inverse-transpose
rows for normals, avoiding dependence on host and shader matrix storage conventions.

**Texture and sampler dedup.** `importMaterials` registers one `TextureData` per image/color-space
pair (with its full mip chain) and one `TextureSampler` per distinct wrap/filter state. The same image
used as both sRGB color and linear data gets separate texture entries. Materials name both by index, so
the shader's bindless arrays are filled in exactly the order the importer registered them.

## Host/device contract

[scene_io.h.slang](../shaders/scene_io.h.slang) is included by both the C++ and the Slang side, so
the two definitions cannot drift and each size is asserted in one place (Slang has no `sizeof`
type operand, so the assertions are host-only; the shared definition is what keeps layouts equal).

| Struct | Size | Role |
| --- | --- | --- |
| `Primitive` | 20 B | `firstTriangle`, `triangleCount`, `material`, `vertexBase`, `vertexCount` |
| `Instance` | 112 B | Object-to-world rows, normal rows, `primitive`, `material`, `lightBase`, `flags` |
| `Light` | 96 B | Triangle vertices and material/UVs, or point position/intensity; `info.y` identifies the type |
| `GltfMaterial` | 160 B | PBR/volume factors, flags, texture/sampler indices, normal/occlusion scales, and attenuation parameters |
| `Params` | 112 B | Per-dispatch constants (image, integrator, camera basis, environment) |
| `RasterParams` | 112 B | Raster constants: view-projection rows, eye + `tan(fov/2)`, environment + eye-to-target distance, total light count + debug view + triangle light count |

Not shared are the vertex attributes. Positions, normals, UVs and indices are each bound as their
own typed array (`StructuredBuffer<float3>`, `<float2>`, `<uint3>`), which is what lets the elements
keep their tight glTF sizes instead of being padded to `float4` — the padding was only ever needed to
make a `float3` struct member unambiguous.

`SNR_MAX_TEXTURES` (1024) and `SNR_MAX_SAMPLERS` (32) live here too. The host sizes its bindless
descriptor pool from these constants, so the shader's handle-array bounds and the pool capacity
cannot disagree.

Params packing ([session.cpp](../src/render/session.cpp)):

| Field | Contents |
| --- | --- |
| `image` | width, height, accumulated sample base, batch sample count |
| `integrator` | max bounces, RNG seed, light count, NEE enabled |
| `eye` | camera position, and `tan(fov/2)` in `.w` |
| `forward` | normalized view direction, aspect ratio in `.w` |
| `right`, `up` | orthonormal camera basis |
| `environment` | constant environment radiance; `.w` is zero for tracing and exposure for tone-map readback |

## GPU resource details

### Geometry buffers

`positions` and `indices` are uploaded once with
`BufferUsage::ShaderResource | BufferUsage::AccelerationStructureBuildInput`.
All other scene buffers use `ShaderResource`.

### Textures and samplers

Each `TextureData` becomes one `ITexture` with its full mip chain (RGBA8, sRGB variant when the
image is a colour texture), an `ITextureView`, and a descriptor handle pulled from the view. Each
`TextureSampler` becomes one `ISampler` with a matching handle. The handles are written into the
shader's bindless `images[]`/`samplers[]` arrays at the index the material names. The `ITexture`,
`ITextureView` and `ISampler` objects are retained for the renderer's lifetime, because the
descriptor handle is owned by them.

### Acceleration structures

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

### Binding

`GpuScene::bind()` distinguishes the two binding models:

- **Buffers** (`sceneBVH`, `positions`, `normals`, `baseColorUvs`, `metallicRoughnessUvs`,
  `normalUvs`, `occlusionUvs`, `emissiveUvs`, `transmissionUvs`, `indices`, `primitives`, `instances`, `materials`,
  `lights`, `triangleLights`, `lightCdf`) are bound by **name** into descriptor set 0.
- **Textures and samplers** are written into their **bindless slots** by `setDescriptorHandle`, one
  per registered image/sampler.

`GpuScene::bindRaster()` binds the same buffers **except** `triangleLights`, which the raster program
never declares. `sceneBVH` *is* bound: the fragment shader's shadow rays are inline queries against the
same top-level structure the path tracer uses. `GpuScene` also exposes one `InstanceDraw` per instance
for the [raster draw loop](#pass-and-draw-loop) — the primitive and its triangle count (which together give the draw's vertex
count), whether the instance's material is glTF BLEND (which picks the draw's group), and the
instance's world origin (which orders the transparent group).


The shared material header accepts the including module's bindless arrays as function parameters.
The tone-map header reads the including module's `film`, `display` and `params` declarations.
Neither header declares independent bindings.

## Shading details

The [path loop](rendering-flow.md#3-one-path-inside-a-gpu-thread) owns execution order.
This section records the material, visibility and sampling details used by that loop.

### Materials and BSDF

`materialAt` produces a `SurfaceMaterial` (base color, emission, metallic, roughness, occlusion) by
multiplying the glTF factors with the matching textures. The model is metallic-roughness:

- Dielectric diffuse, reflection and transmission lobes are selected per hit; metals retain reflection.
- Specular uses a GGX distribution with VNDF sampling and height-correlated Smith visibility, and
  Schlick Fresnel.
- Sub-milliperceptual roughness is treated as a **delta** mirror lobe. Its delta sample flag prevents
  applying continuous-density emitter-hit MIS to the next hit.
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

### Traversal and hit resolution

`traceScene` runs an inline RayQuery that skips procedural primitives. Candidate triangles pass
through `acceptSurfaceHit` for alpha and facing rules; volume entry and exit boundaries remain
traceable regardless of the material's double-sided flag. A committed hit carries distance,
instance ID, local triangle index and barycentrics. Mirrored-instance facing uses world-space
normals derived from transformed edges.

### Light sampling

Emissive geometry is flattened into world-space `Light` triangles during `finalize`, each carrying
its material index and its emissive UVs so a textured emitter can be read at the sampled point. Each instance owns a run in `triangleLights`,
with a light index or -1 for every local triangle. A hit resolves its light as
`triangleLights[instance.lightBase + localTriangle]`. Two things keep the list balanced:

- **Pruning.** A triangle whose emissive texture is black at its centroid is not added to the list.
  It still emits when a BSDF sample hits it (the emission term is unchanged); it is simply not a NEE
  target, so a mostly-black emissive map does not bury the real lights.
- **Power weighting.** Each light is weighted by its emitted radiance times its world-space area, and
  `lightCdf` is the prefix sum. `lightIndexFromCdf` draws a uniform value and reads the light off the
  interval it lands in, so a bright light is sampled more often than a dim one. `lightPdf` scales the
  uniform-on-the-triangle density by that selection probability, and `triangleLights` maps
  a hit back to its light so a directly hit emitter is weighted by exactly the same density.

Analytic `Scene::pointLights` are appended after the triangle prefix during `finalize()`. They have
no geometry and store position in `Light::p0.xyz`, radiant intensity in `p1.rgb`, and `lightTypePoint`
in `info.y`. Their CDF weights are proportional to `4 * luminance(intensity)` (the common pi factor
is omitted from both triangle and point power weights). NEE uses the discrete light-selection
probability, inverse-square falloff, and MIS weight 1: a BSDF sample cannot hit a zero-area point.
Point lights require NEE; disabling NEE leaves environment and emissive geometry contributions.

The studio rig creates a floor and one point light at the former area emitter's center. Its intensity
is the former radiance times quad area, `36 * radius^2`, to preserve the far-field intensity along
the old emitter's normal and keep brightness comparable across model scales. Shadows are hard.

### Sampling utilities

`hash32` builds the per-sample RNG state from pixel, sample index and seed; `randomFloat` is a
small counter-based PRNG. `offsetRay` nudges ray origins off the surface to avoid self-intersection.

## Raster implementation

The preview uses one forward pass with no G-buffer. Its host implementation is
[rasterizer.cpp](../src/render/rasterizer.cpp), with the shader entries listed in the code map.

### Pass and draw loop

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

### Vertex pulling

`rasterVS` reads `SV_VertexID` and derives `triangle = vertexId / 3`, `corner = vertexId % 3`, then
addresses `indices[primitive.firstTriangle + triangle] + primitive.vertexBase`. Because each
primitive owns a contiguous vertex window, this resolves a corner with no index buffer, no
vertex-buffer usage change, and no second copy of position data — the [shared geometry contract](#hostdevice-contract) holds.
The vertex is transformed by `RasterParams.viewProjRows` and handed on as `SV_Position`, with the
world position and the instance index passed through as varyings. The projection rows reproduce the
path tracer's ray basis, so the two renderers frame a model identically.

### Fragment shading

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
only available in raster mode; controls are listed under [usage](../README.md#interactive-controls).

### Transfer into Film

`copyTextureToBuffer` transfers the linear color target into Film accumulation as one sample.
This requires `CopySource` on the texture and `CopyDestination` on the accumulation buffer.
Subsequent [window presentation or file export](rendering-flow.md#two-output-paths) uses the shared output paths.

## Presentation implementation

`Session::present()` loads present.slang lazily and caches a graphics pipeline for the swapchain format.
`PresentParams::image.xy` holds film dimensions and `.zw` output dimensions.
Bilinear sampling fits the linear film with preserved aspect ratio and black bars.
See [output encoding](rendering-flow.md#two-output-paths) for how the target format affects the shader.

Window resize requests convert framebuffer pixels to GLFW screen coordinates before the next event
pump reconfigures the surface. Render resolution and window dimensions can therefore differ.

Dear ImGui's docking release uses `ImGuiPlatformIO` callbacks to manage each detached panel's GLFW
window and slang-rhi surface. Draw data uses the viewport's display origin and framebuffer scale.
Release the native surface before its GLFW window. Platform windows continue updating when the
main surface is minimized. For panel operations and controls, see [usage](../README.md#interactive-controls).

## Transmission and volume transport

The importer supports `KHR_materials_transmission`, `KHR_materials_volume` and `KHR_materials_ior`.
GltfMaterial stores transmission, IOR, thickness classification, attenuation color/distance and texture
metadata in the shared layout. Transmission reads the linear red channel using its own UV stream.
The default IOR is 1.5; omitted attenuation distance means no absorption.

A nonzero thicknessFactor classifies a closed volume. Thin sheets use a mirrored-hemisphere
transmission lobe without macroscopic refraction. The rough BTDF uses GGX visible-normal sampling,
the refractive Jacobian and a radiance eta-squared factor; it supports Fresnel reflection and total
internal reflection. Transmission and diffuse share the dielectric layer beneath reflection.
Metals only reflect.

medium.slang tracks up to 16 nested homogeneous volumes. Absorption is
`exp(-sigma_a * worldSpaceDistance)`; only transmitted boundary crossings update the medium stack.
Both ray-based renderers preserve thicknessTexture metadata for inspection but ignore its baked
distance when transporting light. Volume meshes must be closed with consistent outward winding.
Entry and exit faces remain traceable even when doubleSided is false. Neither extension adds
participating-medium scattering.

Raster follows at most 12 transmission boundaries, shades reached surfaces with direct/ambient light,
uses fixed pixel-seeded roughness samples and explicit secondary-ray texture LOD. Binary shadows,
missing full reflection paths and no refractive caustic solver limit it to a preview.
For glass reference images use path tracing with sufficient samples. Regression checks and commands
are documented under [verification](../README.md#verification).
