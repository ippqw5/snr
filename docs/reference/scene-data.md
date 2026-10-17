# Scene data

Read this after the architecture overview when changing scene import or CPU data representation.
This reference defines geometry addressing, attribute arrays, transforms and texture registration.

[Architecture](../architecture.md) · [Project README](../../README.md)

## Geometry concepts

Keep these three geometry concepts distinct:

| Concept | Meaning here | GPU use |
| --- | --- | --- |
| Vertex / triangle | Vertex attribute arrays and triangles containing three local vertex indices | Shaders fetch attributes; AS construction reads positions / indices |
| `Primitive` | A triangle range with one material and its own vertex window | One BLAS is built per primitive |
| `Instance` | A transformed placement of a primitive in the world | The TLAS places instances; hits resolve geometry and material through them |

A BLAS accelerates intersection with one primitive. The TLAS assembles those BLASes using instance
transforms to represent the scene. Triangle indices are relative to the primitive's `vertexBase`;
they cannot be used directly as global vertex indices.

## Scene fields

`Scene` ([scene.h](../../src/scene/scene.h)) is the entire CPU-side model. Geometry is stored as
**typed arrays in structure-of-arrays form**, not as per-triangle records:

| Field | Meaning |
| --- | --- |
| `positions`, `normals` | One `vec3` per vertex. `normals` is zero when the source had no usable normal. |
| `baseColorUvs`, `metallicRoughnessUvs`, `normalUvs`, `occlusionUvs`, `emissiveUvs`, `transmissionUvs` | One `vec2` per vertex, one array per material texture slot because every slot may use a different UV set. |
| `indices` | One `uvec3` per triangle. The ids are **local to the owning primitive's vertex window**. |
| `primitives` | `Primitive` records: one triangle range with a single material. |
| `instances` | `Instance` records: a transform plus the primitive it places. |
| `materials`, `lights` | `GltfMaterial` and world-space `Light` records. |
| `triangleLights`, `lightCdf` | Per-triangle light remap and the light power distribution; see [light sampling](light-transport.md#light-sampling). |
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
(see [AS constraints](gpu-resources.md#acceleration-structures)), so a primitive must not reference vertices outside
its own triangles.

**Transforms.** `addInstance` stores three explicit object-to-world rows and three inverse-transpose
rows for normals, avoiding dependence on host and shader matrix storage conventions.

**Texture and sampler dedup.** `importMaterials` registers one `TextureData` per image/color-space
pair (with its full mip chain) and one `TextureSampler` per distinct wrap/filter state. The same image
used as both sRGB color and linear data gets separate texture entries. Materials name both by index, so
the shader's bindless arrays are filled in exactly the order the importer registered them.
