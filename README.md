# snr — Slang Neural Renderer

snr is a research-oriented renderer written in C++20 and Slang. It currently loads glTF scenes and
renders them on the GPU — headless by default, or in an interactive raster window with `--window`.
The long-term goal is to fold neural rendering techniques into the renderer itself — denoising,
super resolution, frame generation, neural textures, neural materials and neural radiance caching —
implemented in pure Slang so that both training and inference stay on the GPU without a PyTorch
dependency.

The project is under active development. What follows describes what is implemented today, how the
code is organized, and where it is heading.

## Status

| Area | State |
| --- | --- |
| glTF 2.0 loading | Working (`.gltf` and `.glb`) |
| GPU path tracing | Working (hardware ray queries, progressive accumulation) |
| GPU forward raster preview | Working (single pass, for quick previews) |
| Neural post-pipeline | Planned |
| Neural in-pipeline | Planned |

## Current capabilities

Rendering runs on Vulkan through [slang-rhi](https://github.com/shader-slang/slang-rhi), using
hardware ray queries rather than a software traversal. Two rendering paths share the same scene,
camera, film and image-output code: a path tracer and a forward raster preview.

- Two rendering paths: the path tracer (default) and a single-pass forward rasterizer selected with
  `--raster`. The rasterizer shades each surface once in a fragment shader — emissive, an
  environment-radiance ambient term and an analytic light loop whose shadow rays are traced inline
  against the scene's acceleration structure — so it gives an immediate preview without the cost of
  tracing, at the price of no indirect light. glTF alpha modes are honoured: MASK cuts off below the
  material's cutoff, and BLEND instances are drawn after everything else, sorted back to front and
  blended over the result. The pass also writes an object-id target naming the instance under each
  pixel.
  With `--window` the raster preview runs interactively in a GLFW window (mouse orbit/pan/dolly).
  Both paths evaluate materials through one shared Slang module and leave the image pipeline through
  one shared tonemap, so they cannot drift apart.
- glTF 2.0 import via tinygltf: triangle meshes, PBR metallic-roughness materials, and the core
  material texture slots — base color, metallic-roughness, normal, occlusion and emissive — with
  per-slot UV sets and UV transforms, alpha modes (`OPAQUE`/`MASK`/`BLEND` and `alphaCutoff`), node
  hierarchy with instancing, and selection among multiple glTF scenes.
- An optional studio rig: a ground plane and an area light that can be added to a loaded model with
  `--studio`.
- Integrator: path tracing with next-event estimation (NEE) for emissive triangles, multiple
  importance sampling (power heuristic) between BSDF and light sampling, GGX/VNDF specular with a
  Smith visibility term, cosine-weighted diffuse, and Russian roulette after the third bounce. Lights
  are sampled in proportion to their emitted power, and a textured emitter contributes only its
  non-black triangles so small emitters do not dilute the sampling.
- Material detail: normal mapping uses Mikkelsen's derivative-free tangent frame, built from each
  triangle's world-space edges and its normal-map UVs because ray-traced hits have no screen-space
  derivatives; the occlusion map scales the diffuse lobe.
- Progressive rendering: samples accumulate into a film buffer in batches, so a session can be
  resumed or extended up to a target sample count.
- Output: tone-mapped 8-bit PNG plus a full-precision linear HDR image, with a configurable exposure
  in stops.

## Architecture

The code is split so that CPU-side scene description, GPU resources, and the render loop are
independent of one another. A detailed walkthrough of the scene data model, the host/device
boundary, and the path-tracing algorithm is in [docs/architecture.md](docs/architecture.md).

```
src/
  gpu/        DeviceContext (Vulkan device, queue, validation) and ShaderLibrary
              (Slang module loading, compute pipeline cache), plus buffer helpers
  scene/      Scene (CPU-side geometry/material/light data), GltfImporter
              (glTF -> Scene), GpuScene (buffer upload and acceleration structures),
              sceneJson (scene serialization for --inspect)
  render/     Session (shared scene snapshot, film, settings, tone-map/readback) with
              PathSession (synchronous progressive path tracer) and RasterSession
              (single-pass forward raster preview), Film (accumulation and display
              buffers), shared RenderSettings/RenderResult, render() one-shot entry
  app/        Window (GLFW window + slang-rhi swapchain surface and input polling)
              for the interactive preview
  io/         PNG and HDR image output
  util/       CommandLine (option registry that owns both parsing and usage) and a
              minimal JSON writer
  main.cpp    snr command-line front end (owns main)
shaders/      Slang sources compiled at runtime by ShaderLibrary
cmake/        dependency wiring (Slang from the Vulkan SDK or a source build)
```

### Rendering flow

1. `main.cpp` parses the command line, imports a glTF model into a `Scene`, and calls
   `render()`.
2. `render()` creates a `DeviceContext`, loads the shader module for the selected mode into a
   `ShaderLibrary`, and opens a `PathSession` (path tracing) or a `RasterSession` (with `--raster`).
   Both derive from `Session`, which owns the scene snapshot, the film and the tone-map/readback step.
   With `--window`, `Window` opens a GLFW window and its slang-rhi surface, and the raster session
   draws into the acquired swapchain image each frame instead of writing one image and exiting.
3. The session uploads the scene through `GpuScene`, which builds one bottom-level acceleration
   structure per primitive and a single top-level structure holding every instance. The raster path
   reuses the same buffers and skips the acceleration structures.
4. Path tracing: `render()` dispatches the `traceMain` compute shader in batches, accumulating
   radiance into the film. Raster: `render()` records one draw per instance into an offscreen colour
   and depth target, then copies the colour target into the film. Either way `readback()` runs
   `tonemapMain` on the shared base and copies the accumulation and display buffers back to the host,
   where `saveImages()` writes the results.

### Shaders

| File | Role |
| --- | --- |
| `pathtracer.slang` | Entry point `traceMain` (path tracing); imports the modules below |
| `rasterizer.slang` | Entry points `rasterVS`/`rasterFS` (forward raster preview); same shared modules |
| `scene.slang` | BVH traversal, hit resolution, material lookup and light sampling |
| `material_eval.h.slang` | Texture fetch, corner interpolation, normal mapping and material evaluation, shared by both pipelines as plain functions over the bindless arrays |
| `tonemap.h.slang` | Exposure, tone mapping and sRGB encode, shared by both pipelines |
| `bsdf.slang` | Metallic-roughness BSDF evaluation and sampling |
| `sampling.slang` | RNG, cosine-hemisphere sampling, power heuristic, ray offsetting |
| `scene_io.h.slang` | Structs shared between host and device |

`material_eval.h.slang` and `tonemap.h.slang` declare no bindings of their own; the including module
declares the bindless image/sampler arrays and the `film`/`display` buffers and passes them in. That
is what lets one header serve both the ray-traced and the rasterized pipeline.

`scene_io.h.slang` is the single source of truth for the host/device data layout. Both the C++ side
and the Slang side include the same file, so the two definitions cannot drift; each struct's size is
asserted in one place. `Primitive`, `Instance`, `Light`, `GltfMaterial` and `Params` are all defined
there. Vertex attributes are not: positions, normals and UVs are bound as their own tight typed
arrays, so an `float3`/`float2` element needs no `float4` padding to cross the boundary.

## Building

Requirements:

- CMake 3.25 or newer and Ninja
- A C++20 compiler
- The Vulkan SDK, with the Slang compiler present (`slangc`) and `VULKAN_SDK` set
- Git submodules checked out

```sh
git submodule update --init --recursive
cmake --preset default
cmake --build build
```

Two configure presets are provided:

- `default` — uses the Slang package shipped with the Vulkan SDK.
- `slang-source-build` — uses a Slang source build at `../slang/build-snr/Release/install`.

On Windows the Vulkan SDK ships Slang's binaries and headers but not its CMake package, so
`cmake/Dependencies.cmake` reconstructs the `slang::slang` and `slang::slangc` targets from that
layout. If you build Slang yourself, point `-Dslang_DIR` at its `lib/cmake/slang`.

## Usage

The `snr` executable is the only front end.

```sh
# A Khronos sample asset, lit with a studio rig
snr --model DamagedHelmet --studio --width 1280 --height 720

# The same scene through the forward raster preview (direct lighting with shadows, immediate)
snr --model DamagedHelmet --studio --raster --width 1280 --height 720

# Interactive raster preview in a window: left drag orbits, right drag pans, wheel dollies
snr --model DamagedHelmet --studio --window --width 1280 --height 720

# A model from an explicit path
snr --model ./my-scene.glb --spp 512 --output output/my-scene.png

# List the models available under the asset directory
snr --list-models

# Dump the derived scene as JSON, without creating a GPU device
snr --model DamagedHelmet --inspect
```

Common options:

| Option | Meaning |
| --- | --- |
| `--model NAME_OR_PATH` | Khronos sample-asset name, or a path to a `.gltf`/`.glb` (required) |
| `--asset-dir PATH` | Directory used for model-name lookup |
| `--width N --height N` | Image resolution (default 800x800) |
| `--spp N --batch-size N` | Target samples per pixel and per-dispatch batch size |
| `--max-bounces N --seed N` | Integrator depth and RNG seed |
| `--exposure STOPS` | Display exposure in stops |
| `--camera X Y Z --target X Y Z --fov DEG` | Camera override |
| `--environment VALUE` | Constant linear environment radiance |
| `--studio` | Add a floor and area light to a glTF model |
| `--raster` | Preview with the forward rasterizer instead of path tracing |
| `--window` | Show the raster preview in an interactive window (implies `--raster`) |
| `--no-nee` | Disable next-event estimation |
| `--shader-dir PATH` | Override the shader source directory |
| `--validate` | Enable RHI and Vulkan validation |
| `--inspect` | Print the derived scene as JSON and exit, without a GPU device |
| `--help` | Print the option list |

## Roadmap: neural rendering in pure Slang

The design constraint that shapes the roadmap is keeping the neural components inside the shader
language. Slang provides automatic differentiation, which makes it possible to define small networks
and train them on the device alongside the renderer, rather than exporting data to PyTorch. The
planned features fall into two integration surfaces.

### Post-pipeline

Neural stages that consume rendered frames and buffers, sitting between the integrator and the final
image:

- Denoising of low-sample renders
- Super resolution / upscaling
- Frame generation and interpolation

These are the least invasive to add: they operate on the film and display buffers that `Film`
already exposes, and can be layered on top of the existing `tonemapMain` stage.

### In-pipeline

Neural components that participate directly in light transport:

- Neural textures — learned, compressed surface detail
- Neural materials — learned BSDFs replacing hand-authored lobes
- Neural radiance cache (NRC) — a learned cache of radiance to reduce path-tracing cost

These require the integrator in `pathtracer.slang` and the BSDF in `bsdf.slang` to become
extensible, and they share the same differentiable-Slang approach as the post-pipeline work.

## Dependencies

| Dependency | Purpose |
| --- | --- |
| [Slang](https://github.com/shader-slang/slang) | Shader language and compiler |
| [slang-rhi](https://github.com/shader-slang/slang-rhi) | GPU abstraction over Vulkan |
| [tinygltf](https://github.com/syoyo/tinygltf) | glTF 2.0 parsing |
| [glm](https://github.com/g-truc/glm) | Vector and matrix math |
| [GLFW](https://github.com/glfw/glfw) | Interactive window (fetched by CMake at configure time) |
| [stb](https://github.com/nothings/stb) | Image writing |
| [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) | Test models |
| Vulkan SDK | Device, ray queries, validation |
