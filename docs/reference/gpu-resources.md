# GPU resources and host/device contracts

Read the architecture's upload and rendering sections first. Use this reference when changing
shared records, acceleration structures or shader bindings.

[Architecture](../architecture.md) · [Project README](../../README.md)

## Host/device contract

[scene_io.h.slang](../../shaders/scene_io.h.slang) is included by both the C++ and the Slang side, so
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

Params packing ([session.cpp](../../src/render/session.cpp)):

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
`ITextureView` and `ISampler` objects are retained by `GpuScene`, because the descriptor handle is owned by them.
See [ownership and lifetime](../architecture.md#ownership-and-lifetime) for the surrounding resource lifetimes.

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
for the [raster draw loop](rasterizer.md#pass-and-draw-loop) — the primitive and its triangle count (which together give the draw's vertex
count), whether the instance's material is glTF BLEND (which picks the draw's group), and the
instance's world origin (which orders the transparent group).


The shared material header accepts the including module's bindless arrays as function parameters.
The tone-map header reads the including module's `film`, `display` and `params` declarations.
Neither header declares independent bindings.

## Shader library

[ShaderLibrary](../../src/gpu/shader_library.cpp) caches modules and pipelines. Graphics pipelines
are keyed by entry-point pair and variant name, so opaque, masked and blended raster states can
share entry points.
