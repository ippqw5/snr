# snr — Slang Neural Renderer

snr is a research-oriented, headless renderer written in C++20 and Slang. It currently loads glTF
scenes and path-traces them on the GPU. The long-term goal is to fold neural rendering techniques
into the renderer itself — denoising, super resolution, frame generation, neural textures, neural
materials and neural radiance caching — implemented in pure Slang so that both training and
inference stay on the GPU without a PyTorch dependency.

The project is under active development. What follows describes what is implemented today, how the
code is organized, and where it is heading.

## Status

| Area | State |
| --- | --- |
| glTF 2.0 loading | Working (`.gltf` and `.glb`) |
| GPU path tracing | Working (hardware ray queries, progressive accumulation) |
| Neural post-pipeline | Planned |
| Neural in-pipeline | Planned |

## Current capabilities

Rendering runs on Vulkan through [slang-rhi](https://github.com/shader-slang/slang-rhi), using
hardware ray queries rather than a software traversal.

- glTF 2.0 import via tinygltf: triangle meshes, PBR metallic-roughness materials, base-color and
  metallic-roughness textures with per-slot UV sets and UV transforms, node hierarchy with
  instancing, and selection among multiple glTF scenes.
- Built-in scenes for validation: a Cornell box, a furnace test, and an optional studio rig (ground
  plane plus area light) that can be added to a loaded model.
- Integrator: path tracing with next-event estimation (NEE) for emissive triangles, multiple
  importance sampling (power heuristic) between BSDF and light sampling, GGX/VNDF specular with a
  Smith visibility term, cosine-weighted diffuse, and Russian roulette after the third bounce.
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
  render/     RenderSession (synchronous progressive renderer), Film (accumulation
              and display buffers), RenderSettings/RenderResult, render() one-shot entry
  io/         PNG and HDR image output
  util/       CommandLine (option registry that owns both parsing and usage) and a
              minimal JSON writer
  pathtracer.cpp  pathtracer command-line front end (owns main)
shaders/      Slang sources compiled at runtime by ShaderLibrary
cmake/        dependency wiring (Slang from the Vulkan SDK or a source build)
```

### Rendering flow

1. `pathtracer.cpp` parses the command line, imports a glTF model into a `Scene`, and calls
   `render()`.
2. `render()` creates a `DeviceContext`, loads `pathtracer.slang` into a `ShaderLibrary`, and opens a
   `RenderSession`.
3. The session uploads the scene through `GpuScene`, which builds one bottom-level acceleration
   structure per primitive and a single top-level structure holding every instance.
4. `renderTo()` dispatches the `traceMain` compute shader in batches, accumulating radiance into the
   film. `readback()` runs `tonemapMain` and copies the accumulation and display buffers back to the
   host, where `saveImages()` writes the results.

### Shaders

| File | Role |
| --- | --- |
| `pathtracer.slang` | Entry points `traceMain` (path tracing) and `tonemapMain`; imports the modules below |
| `scene.slang` | BVH traversal, hit resolution, texture sampling, material lookup, normal mapping, light selection and PDFs |
| `bsdf.slang` | Metallic-roughness BSDF evaluation and sampling |
| `sampling.slang` | RNG, cosine-hemisphere sampling, power heuristic, ray offsetting |
| `scene_io.h.slang` | Structs shared between host and device |

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

The `pathtracer` executable is the only front end.

```sh
# A Khronos sample asset, lit with a studio rig
pathtracer --model DamagedHelmet --studio --width 1280 --height 720

# A model from an explicit path
pathtracer --model ./my-scene.glb --spp 512 --output output/my-scene.png

# List the models available under the asset directory
pathtracer --list-models

# Dump the derived scene as JSON, without creating a GPU device
pathtracer --model DamagedHelmet --inspect
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
| [stb](https://github.com/nothings/stb) | Image writing |
| [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) | Test models |
| Vulkan SDK | Device, ray queries, validation |
