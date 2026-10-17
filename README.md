# snr — Slang Neural Renderer

snr is a C++20 / Slang research renderer for glTF scenes on Vulkan through
[slang-rhi](https://github.com/shader-slang/slang-rhi).
Launch without arguments or with `--window` for the GUI, or supply a model without `--window`
for headless rendering.

The long-term goal is neural rendering in pure Slang, with GPU training and inference without
a PyTorch dependency. See the [roadmap](#roadmap).

## Documentation

New to the project? [Build and run it](#building), then read the
[architecture](docs/architecture.md) from the system overview through scene preparation,
rendering, output and state changes. Follow its topic links when you need implementation details.

| Need | Read |
| --- | --- |
| Understand the system and follow a scene to an image | [Architecture](docs/architecture.md) |
| Change scene import or CPU representation | [Scene data](docs/reference/scene-data.md) |
| Change shared layouts, GPU resources or bindings | [GPU resources](docs/reference/gpu-resources.md) |
| Understand path tracing, materials, sampling or volumes | [Light transport](docs/reference/light-transport.md) |
| Change Raster or window presentation | [Rasterizer](docs/reference/rasterizer.md) / [Presentation](docs/reference/presentation.md) |
| Test renderer changes | [Renderer tests](tests/README.md) |
| Modify the project with an AI agent | [Repository rules and workflow](AGENTS.md) |

## Status

| Area | Current state |
| --- | --- |
| Scene import | glTF 2.0 (`.gltf` / `.glb`), instancing, PBR textures, per-slot UV transforms and alpha modes |
| Path tracing | Progressive hardware ray queries, NEE, MIS, reflection and transmission |
| Raster preview | Single-pass forward shading, direct-light shadows and debug views |
| Materials | Transmission, volume absorption and IOR extensions; [semantics and limits](docs/reference/light-transport.md#transmission-and-volume-transport) |
| Utilities | Optional studio floor / point light (`--studio`), tone-mapped PNG and linear HDR output |
| Neural rendering | Planned; see the [roadmap](#roadmap) |

## Building

Requirements:

- CMake 3.25 or newer and Ninja
- A C++20 compiler
- The Vulkan SDK, with the Slang compiler present (`slangc`) and `VULKAN_SDK` set
- Git submodules checked out; all third-party source and asset dependencies live under `external/` as pinned submodules

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

# Interactive raster preview: left drag orbits, right drag pans, wheel dollies, keys 1-5 switch views
snr --model DamagedHelmet --studio --window --width 1280 --height 720

# Open the UI, then import a local .gltf or .glb through the file browser
snr --window --width 1280 --height 800

# A model from an explicit path
snr --model ./my-scene.glb --spp 512 --output output/my-scene.png

# List the models available under the asset directory
snr --list-models

# Dump the derived scene as JSON, without creating a GPU device
snr --model DamagedHelmet --inspect
```

For GUI, PNG and HDR encoding differences, see [presentation and file output](docs/architecture.md#presentation-and-file-output).

### Interactive controls

- **Import glTF / GLB** opens a file browser with path entry, filename filtering and folder navigation.
  Click a model to import it. Loading progress appears in the UI; further imports are disabled until
  completion. Failures appear in the panel and console.
- **Add studio floor and point light** applies to the next import. `--studio` initializes it.
- **Renderer** selects Raster or Path tracing. Path controls expose target SPP, SPP per frame, bounce
  count, direct-light sampling, pause and restart. Exposure applies to both renderers.
  [Accumulation rules](docs/architecture.md#what-invalidates-accumulated-samples) explain which changes restart sampling.
- Left drag orbits, right drag pans, and the wheel changes eye-to-target distance.
  ImGui-captured input does not move the camera. Keys **1–5** select raster shaded, depth,
  object-id, base-color and normal views.
- **Resolution** shows framebuffer and render sizes. **Follow window size** is enabled by default.
  **Apply window size** changes the main drawable size; disable following and use **Apply resolution**
  for a fixed film size. Both accept 1–4096 pixels per axis; display fitting preserves aspect ratio.
- Drag panel titles to dock or detach them. **Panels** reopens closed panels and toggles the fixed
  bottom-right world axes (red X, green Y, blue Z). Detached panels update while the main window is
  minimized; closing the main window closes the GUI.
- The window title is `snr gui`. Scene preview shows the glTF scene name (or model filename),
  with the full path on hover, FPS, frame time and frame count.
  **System Information** shows the graphics API, GPU and CPU.

### Command-line options

| Option | Meaning |
| --- | --- |
| `--model NAME_OR_PATH` | Khronos sample-asset name, or a path to a `.gltf`/`.glb` (optional in the UI) |
| `--asset-dir PATH` | Directory used for model-name lookup |
| `--width N --height N` | Image resolution (default 800x800) |
| `--spp N --batch-size N` | Target samples per pixel and per-dispatch batch size |
| `--max-bounces N --seed N` | Integrator depth and RNG seed |
| `--exposure STOPS` | Display exposure in stops |
| `--camera X Y Z --target X Y Z --fov DEG` | Camera override |
| `--environment VALUE` | Constant linear environment radiance |
| `--studio` | Add a floor and point light to a glTF model |
| `--raster` | Preview with the forward rasterizer instead of path tracing |
| `--window` | Interactive GUI with glTF import, Raster / Path tracing selection, sampling controls and XYZ axes |
| `--no-nee` | Disable next-event estimation |
| `--output PATH.png` | Headless PNG output path; also writes a linear HDR file with the same basename |
| `--shader-dir PATH` | Override the shader source directory |
| `--validate` | Enable RHI and Vulkan validation |
| `--inspect` | Print the derived scene as JSON and exit, without a GPU device |
| `--help` | Print the option list |

## Roadmap

Planned work keeps both training and inference in Slang, using its automatic differentiation:

| Integration point | Planned features |
| --- | --- |
| After rendering | Denoising, super resolution, frame generation |
| Inside light transport | Neural textures, neural materials, neural radiance caching |

## Dependencies

| Dependency | Purpose |
| --- | --- |
| [Slang](https://github.com/shader-slang/slang) | Shader language and compiler |
| [slang-rhi](https://github.com/shader-slang/slang-rhi) | GPU abstraction over Vulkan |
| [tinygltf](https://github.com/syoyo/tinygltf) | glTF 2.0 parsing |
| [glm](https://github.com/g-truc/glm) | Vector and matrix math |
| [GLFW](https://github.com/glfw/glfw) | Interactive window (Git submodule) |
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI controls and file browser (Git submodule) |
| [stb](https://github.com/nothings/stb) | Image writing |
| [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) | Test models |
| Vulkan SDK | Device, ray queries, validation |
