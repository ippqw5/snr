# GPU resources and host/device contracts

`GpuScene` bridges the CPU scene representation and the resources used by both renderers. Read
the architecture's [upload flow](../architecture.md#uploading-a-gpu-snapshot) and
[ownership model](../architecture.md#ownership-and-lifetime) for construction and lifetime.

## Host/device contract

C++ and Slang include the same scene record definitions. Geometry, instances, materials, lights
and rendering parameters therefore share one source for their layouts and resource limits.
The central records are `Primitive`, `Instance`, `GltfMaterial` and `Light`; `Params` and
`RasterParams` carry the state for each renderer.
The definitions and host size checks live in [scene_io.h.slang](../../shaders/scene_io.h.slang).

Vertex attributes remain separate typed arrays. Together with primitive-local indexing, this
representation gives acceleration structures and shaders a common view of geometry without
requiring a second position or index representation.

## Acceleration structures

Each primitive has a bottom-level acceleration structure (BLAS). A top-level structure (TLAS)
places them using scene instances. This separates reusable geometry from world placement and
connects ray hits to the same instances and materials used by Raster.

BLAS construction uses each primitive's compact vertex window. That boundary matters to both
geometry addressing and the backend's interpretation of build inputs. The build code contains
the formats, offsets, count constraints and synchronization needed by slang-rhi.

## Resource binding

Scene buffers use named shader bindings. Textures and samplers use indexed bindless resources;
their indices preserve the associations established during import. Texture views and samplers
own the descriptor handles, so `GpuScene` retains those objects with its scene resources.

`GpuScene::bind()` and `GpuScene::bindRaster()` expose the same scene snapshot and TLAS.
Path tracing also consumes the triangle-to-light mapping needed to weight light hits.
Raster receives per-instance draw metadata
for its [visibility and transparency design](rasterizer.md#pass-and-draw-loop).

Shader libraries own module and pipeline creation. Distinct graphics states can share shader
entry points while retaining separate pipeline variants. Binding declarations, constants and
cache keys are defined alongside their consumers in code.

## Code entry points

| Source | What to look for |
| --- | --- |
| [scene_io.h.slang](../../shaders/scene_io.h.slang) | Shared records, sizes and limits |
| [gpu_scene.h](../../src/scene/gpu_scene.h), [gpu_scene.cpp](../../src/scene/gpu_scene.cpp) | Uploads, resource ownership, AS construction and renderer bindings |
| [scene.slang](../../shaders/scene.slang), [rasterizer.slang](../../shaders/rasterizer.slang) | Resource declarations for each renderer |
| [session.cpp](../../src/render/session.cpp), [dispatch.h](../../src/render/dispatch.h) | Rendering parameters and compute submission |
| [shader_library.cpp](../../src/gpu/shader_library.cpp) | Shader composition and pipeline caches |
