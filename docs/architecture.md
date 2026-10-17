# Architecture

After [building and running the project](../README.md#building), read this document from top to
bottom to understand how a scene becomes an image. It assumes basic C++ and graphics knowledge;
project-specific objects are introduced before their execution details.

This document describes the current implementation. The [roadmap](../README.md#roadmap) records
planned work. Field layouts, shader algorithms and pipeline settings live in the topic references
linked below.

- [System overview](#system-overview)
- [Core objects](#core-objects)
- [From scene to image](#from-scene-to-image)
- [State changes and resource lifetime](#state-changes-and-resource-lifetime)
- [Code map and further reading](#code-map-and-further-reading)

## System overview

snr is a C++20 / Slang renderer for glTF scenes. C++ prepares scene data and submits Vulkan work
through slang-rhi; Slang shaders perform GPU shading. Neural training and inference are planned.

![The entry point selects a scene and render mode. Rendering coordinates the UI and sessions; sessions use GPU services and Slang shaders, then return CPU results for file output.](diagrams/system-overview.svg)

The diagram shows logical responsibilities and work delegation, rather than include dependencies
or submission timing. The main areas are:

| Area | Responsibility |
| --- | --- |
| `src/main.cpp`, `src/util/` | Parse options, select an input model and handle CPU inspection |
| `src/scene/` | Import glTF, derive CPU scene data and create a GPU scene snapshot |
| `src/render/` | Coordinate headless / GUI execution, render sessions and image storage |
| `src/app/` | Manage windows, camera input and UI controls |
| `src/gpu/` | Manage the device, compile shaders at runtime, cache pipelines and submit commands |
| `shaders/` | Evaluate shared materials, trace paths, shade raster fragments and transform output |
| `src/io/` | Write CPU render results to image files |

The GUI starts with Raster for an immediate preview. Path tracing progressively accumulates samples
for indirect lighting and reflection / transmission paths. Both use the same GPU scene representation,
shared material evaluation and Film output format. Their lighting methods differ; Raster remains
a preview approximation. Detailed material support and limits are in
[light transport](reference/light-transport.md#transmission-and-volume-transport).

## Core objects

| Object | Role |
| --- | --- |
| `Scene` | CPU geometry, instances, materials, textures, lights and initial camera |
| `GpuScene` | Uploaded snapshot: GPU buffers, textures, samplers and acceleration structures |
| `Session` | Rendering settings and a Film, with a shared reference to a GpuScene |
| `RasterSession` / `PathSession` | The renderer-specific pipelines and work that fill a Film |
| `Film` | GPU accumulation / display buffers and a CPU sample counter |
| `RenderResult` | CPU copies of an image and its sample metadata, returned by readback |

Scene preparation leads to a GpuScene; a Session renders into its Film; readback produces a
RenderResult when CPU output is needed. These names describe different storage and ownership roles.

![CPU import and derivation produce a GpuScene snapshot. Raster and path sessions share the snapshot while each owns a separate Film.](diagrams/scene-ownership.svg)

Solid arrows show data creation or transfer; dashed arrows show a Session holding a shared
reference. Session and Film are C++ management objects on the CPU. The buffers, textures and
acceleration structures they manage reside on the GPU.

A primitive is a triangle range with one material; an instance places a primitive in the world
with a transform. A bottom-level acceleration structure (BLAS) accelerates one primitive;
a top-level structure (TLAS) assembles them using instance transforms.

## From scene to image

### CPU preparation

`Scene::loadGltf()` delegates file reading, material import and geometry import to `GltfImporter`.
`Scene` holds geometry attribute arrays, primitives, instances, materials, textures and lights,
along with the camera and bounds. The importer retains the parsed glTF model so it can derive
another glTF scene without reading the file again.

`Scene::finalize()` derives lights, their sampling cumulative distribution (CDF) and bounds from
the prepared geometry and instances. The CDF selects lights according to their weights.
The shader consumes these precomputed light records and probabilities; it does
not rebuild a light list per pixel. `--inspect` exports the CPU data before any GPU device exists,
which makes it useful for isolating import problems.

See [scene data](reference/scene-data.md) for primitives, instances and local vertex indexing.

### Uploading a GPU snapshot

The `GpuScene` constructor uploads attributes, materials, lights, textures and samplers and builds
BLAS/TLAS. The same GPU positions and indices buffers serve both acceleration-structure construction
and shader reads. BLAS build commands are recorded first, followed by a barrier and the TLAS build.
Submission waits for completion before temporary scratch buffers are released.

`GpuScene` does not retain its input `Scene&`. Subsequent rendering uses the uploaded snapshot.
A caller may still retain the CPU Scene, but editing it does not update existing GPU resources:
a new or replacement snapshot is required. Sessions manage their camera, integrator settings and
environment values separately, with dedicated setters. Integrator settings control light transport,
including the bounce limit, random seed and next-event estimation (NEE), which samples direct light.

See [GPU resources](reference/gpu-resources.md) for layouts, build inputs and bindings.

### Rendering into Film

Each Film pixel stores **`(radiance sum.rgb, sample count)`**. Radiance is the linear light value
computed for a sample. Display and export divide the sum by the count to obtain the mean.
The two renderers produce this same representation in different ways:

| Renderer | Work that fills Film | Result after the work completes |
| --- | --- | --- |
| Raster | Draw scene instances into a color target, then copy it into accumulation | One sample per pixel, replacing the previous frame |
| Path tracing | Dispatch a compute batch whose threads trace camera samples | Add the batch's samples to the current accumulation |

#### Raster frame

`RasterSession::render()` clears its color, object-id and depth targets, then draws OPAQUE / MASK
instances followed by sorted BLEND instances. Vertex shaders fetch scene geometry; fragment
shaders evaluate shared materials, direct lighting and the transmission preview. Shadow and
transmission queries use the same TLAS as path tracing.

After the pass, a texture-to-buffer copy places linear color into Film with a per-pixel count of one.
The CPU waits for submission to complete before recording one Film sample. Each Raster render
replaces its previous image. Pipeline variants, alpha handling and debug channels are explained in
the [rasterizer reference](reference/rasterizer.md).

#### Path-tracing batch

![The CPU checks the target, packs parameters and submits compute work. GPU threads sample pixels and write Film; after waiting, the CPU advances the count. Window display and file export then take different paths.](diagrams/sample-batch.svg)

There are three nested loops. Understanding their boundaries matters more than memorizing class names:

| Loop | Where it runs | Work per iteration |
| --- | --- | --- |
| Sampling batch | CPU: `PathSession::renderStep()` | Dispatch one compute pass over the entire image |
| Samples for one pixel | GPU: the `i` loop in `traceMain()` | A thread computes this batch's camera samples sequentially |
| Bounces for one sample | GPU: the `bounce` loop in `tracePath()` | Accumulate radiance along the path until termination |

**One displayed frame is not one sample.** The GUI calls renderStep at most once per frame, and a
batch can contain multiple samples per pixel (SPP). For headless output, `PathSession::render()`
repeats renderStep until it reaches the target.

**CPU parameters, bindings and submission.**

Suppose Film already contains 8 SPP, the target is 10 SPP, and the batch size is 4 SPP:

1. `renderStep()` chooses `min(4, 10 - 8) = 2` samples for this batch.
2. `makeParams()` packs camera, environment and integrator values.
   `Params.image = (width, height, 8, 2)`: `.z` is the starting sample index and `.w` is the batch count.
3. `dispatchImage()` creates an encoder, inserts a barrier, begins a compute pass and binds the
   `traceMain` pipeline.
4. `bindParams()` binds constants, `GpuScene::bind()` binds scene resources, and the caller binds
   Film accumulation. Binding establishes shader access; it does not upload the scene again.
5. Dispatch covers `ceil(width / 8) × ceil(height / 8)` thread groups, each containing `8 × 8` threads.
6. After ending the pass and inserting a barrier, `submitAndWait()` submits and waits for the
   graphics queue. Only then does `Film::addSamples(2)` advance the CPU count to 10.

Every compute batch currently includes a CPU wait. Multiple sampling batches are not kept
asynchronously in flight.

**GPU pixel work.**

`traceMain()` rejects threads outside the image extent, then obtains the pixel's accumulation.
For each sample it seeds the RNG from pixel, sample index and seed, jitters the location within
the pixel, generates a ray from the camera basis and calls tracePath.
It adds `float4(radiance, 1)` for each sample and writes the pixel once after the batch loop.

For the per-sample bounce loop and shading algorithms, continue to [light transport](reference/light-transport.md).

The CPU Film counter schedules the next batch; each GPU pixel's `.w` records its actual count.
`saveImages()` checks that the GPU counts agree with the CPU count when exporting.

### Presentation and file output

| Output | GPU work | CPU work |
| --- | --- | --- |
| GUI window | `Session::present()` draws a fullscreen triangle, reading, scaling and tone mapping accumulation into the swapchain image | Draw UI, then call `Window::present()`; regular display does not read the full image back |
| Headless files | `Session::readback()` dispatches `tonemapMain` into Film's display buffer | `Film::readback()` reads accumulation and display; `saveImages()` writes PNG and HDR |

Both paths use `tonemapCore` for exposure and Reinhard mapping.
The GUI relies on hardware encoding for sRGB swapchain targets, and presentFS explicitly encodes
other target formats. Current `tonemapMain` only writes tone-mapped values into display;
`saveImages()` quantizes them directly to PNG bytes without adding sRGB encoding.
Sharing tone mapping therefore does not imply identical window and PNG encoding.
HDR comes from averaged linear accumulation, without display exposure or tone mapping.

Headless rendering writes both the requested `.png` and a `.hdr` with the same basename.
The GUI does not save files on exit. `RenderResult` owns the CPU copies used for file output;
the headless renderer releases its Session before calling `saveImages()`.

For display-pipeline and window implementation details, see [presentation](reference/presentation.md).

## State changes and resource lifetime

### Ownership and lifetime

| Resource | Owner or holder | Lifetime contract |
| --- | --- | --- |
| `DeviceContext` | Headless call or GUI loop | Outlives its shader libraries, sessions, films and window resources |
| `ShaderLibrary` | Headless renderer scope or GUI loop | Outlives sessions that borrow it; sessions also retain their pipelines |
| `Scene` | Import caller; a worker prepares GUI replacements | GpuScene retains no input Scene reference after upload |
| `GpuScene` | Shared by sessions through `shared_ptr` | Remains alive while any session holds it; resources belong to this snapshot |
| `Film` | One Session | Accumulation / display buffers are replaced on resize and released with the Film |
| `RenderResult` | Readback caller | Owns CPU vectors independent of subsequent rendering and GPU resource lifetime |

Interactive mode creates a `RasterSession` first. The first switch to Path tracing creates a
`PathSession` through `Session(shaders, source)`, sharing the GpuScene while allocating its own
Film. Switching renderers therefore reuses geometry, textures and acceleration structures without
mixing the two renderers' pixel results. **Raster also needs the TLAS**: fragment shaders use ray
queries for shadows and transmission preview.

### What invalidates accumulated samples

Samples can be combined only while scene, camera, image size, integrator and other relevant
conditions remain compatible.

| Operation | GPU scene snapshot | Path-tracing Film |
| --- | --- | --- |
| Change camera | Retained | Reset when the value changes |
| Change image size | Retained | Reallocate buffers and reset when the size changes |
| Change integrator or environment | Retained | Reset when the value changes |
| Change exposure | Retained | Preserve samples; update display |
| Raise target SPP / change batch SPP | Retained | Preserve samples and continue |
| Lower target SPP | Retained | Reset only if the new target is below the accumulated count |
| Click Restart | Retained | Reset |
| Switch GUI renderer | Shared snapshot | Separate films; current camera, size and settings determine whether the cached path film can continue |
| Successfully import another model in the GUI | Build a replacement snapshot | Replace RasterSession and discard the old PathSession |
| Call `PathSession::setScene()` | Build a new snapshot for this session | Reset; other sessions retain their original snapshot |

**Reset currently only sets the CPU sample count to zero; it does not immediately clear GPU buffers.**
On the next batch, `Params.image.z == 0` makes traceMain initialize its local sum to zero and
overwrite each pixel. Other batches read the previous accumulation.
If no rendering follows a reset, the old bytes remain in the buffer.

Even when paused, the GUI computes one fresh batch after reset so the displayed image matches
the current state. These rules live in the
[Session / PathSession setters](../src/render/session.cpp),
[Film::reset](../src/render/film.h), [traceMain](../shaders/pathtracer.slang)
and [interactive loop](../src/render/renderer.cpp).

### Switching renderers

Only the active GUI session receives the current camera, resolution and controls. On returning to
a cached PathSession, setters compare those values with its retained state. An unchanged round
trip resumes the existing accumulation (or remains complete if its target was reached). A changed
camera, resolution or integrator resets it on return. Exposure changes preserve samples.

### Replacing a scene

During GUI model import, CPU parsing and texture decoding run on a worker. GPU resource creation
stays on the rendering thread. Only after a replacement RasterSession has been built and the queue
has been waited on does the application replace the old session and discard the old PathSession.
A failed import preserves the working scene and camera.

### Window resources

Dear ImGui's detached panels each have a GLFW window and a slang-rhi surface. Release the native
surface before destroying its GLFW window. Platform windows continue updating while the main
surface is minimized; the GUI loop waits on the queue before shutdown. Window fitting and callback
implementation details are in the [presentation reference](reference/presentation.md).

## Code map and further reading

Use this map to locate the implementation after following the execution above.

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

### Topic references

| Task | Read next |
| --- | --- |
| Change import or CPU scene representation | [Scene data](reference/scene-data.md) |
| Change shared records, uploads, AS builds or bindings | [GPU resources and host/device contracts](reference/gpu-resources.md) |
| Change material fields end to end | [Scene data](reference/scene-data.md), [shared layouts](reference/gpu-resources.md#hostdevice-contract), then [light transport](reference/light-transport.md) |
| Understand one path or change BSDF / light sampling / media | [Light transport](reference/light-transport.md) |
| Change Raster pipeline state or fragment shading | [Rasterizer implementation](reference/rasterizer.md) |
| Change display fitting, swapchain integration or detached panels | [Presentation implementation](reference/presentation.md) |
| Test renderer changes | [Renderer tests](../tests/README.md) |
| Maintain the diagrams | [Diagram sources and regeneration](diagrams/README.md) |
