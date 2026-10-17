# Usage

Build snr using the [build instructions](../README.md#building). The examples below run from the
repository root and use the Windows executable name.

- [Launch modes and examples](#launch-modes-and-examples)
- [Interactive controls](#interactive-controls)
- [Command-line options](#command-line-options)
- [README image](#readme-image)

## Launch modes and examples

With no arguments, or with `--window`, snr opens the GUI in Raster mode. A model supplied without
`--window` is rendered headlessly with Path tracing, unless `--raster` selects the raster preview.

```sh
# Open the UI and import a local .gltf or .glb through the file browser
build/snr.exe --window --width 1280 --height 800

# Start the GUI with a sample model and studio lighting
build/snr.exe --model DamagedHelmet --studio --window --width 1280 --height 720

# Path trace a sample model to PNG and HDR
build/snr.exe --model DamagedHelmet --studio --width 1280 --height 720 --output output/helmet.png

# Render the same scene with the raster preview
build/snr.exe --model DamagedHelmet --studio --raster --width 1280 --height 720 --output output/helmet-raster.png

# Load a model from an explicit path
build/snr.exe --model ./my-scene.glb --spp 512 --output output/my-scene.png

# List models available under the sample-asset directory
build/snr.exe --list-models

# Inspect the derived CPU scene as JSON, without creating a GPU device
build/snr.exe --model DamagedHelmet --inspect
```

`--list-models`, `--inspect` and `--help` do not create a GPU device. Rendered GUI and headless output
require the [GPU features](../README.md#requirements) listed in the README. The GUI does not save
image files on exit. For PNG, HDR and window color-encoding semantics, see
[presentation and file output](architecture.md#presentation-and-file-output).

## Interactive controls

### Import a model

**Import glTF / GLB** opens a file browser with path entry, filename filtering and folder navigation.
Click a model to import it. Loading progress appears in the UI; further imports are disabled until
completion. Failures appear in the panel and console.

**Add studio floor and point light** applies to the next import. `--studio` initializes it.

### Rendering and navigation

**Renderer** selects Raster or Path tracing. Path controls expose target SPP, SPP per frame, bounce
count, direct-light sampling, pause and restart. Exposure applies to both renderers.
[Accumulation rules](architecture.md#what-invalidates-accumulated-samples) explain which changes
restart sampling.

Left drag orbits, right drag pans, and the wheel changes eye-to-target distance. ImGui-captured input
does not move the camera. In Raster mode, keys **1–5** select shaded, depth, object-id, base-color
and normal views; the View selector exposes the same choices.

### Resolution

**Resolution** shows framebuffer and render sizes. **Follow window size** is enabled by default.
**Apply window size** changes the main drawable size; disable following and use **Apply resolution**
for a fixed film size. Both accept 1–4096 pixels per axis; display fitting preserves aspect ratio.

### Panels and diagnostics

Drag panel titles to dock or detach them. **Panels** reopens closed panels and toggles the fixed
bottom-right world axes (red X, green Y, blue Z). Detached panels update while the main window is
minimized; closing the main window closes the GUI.

The window title is `snr gui`. Scene preview shows the glTF scene name (or model filename),
with the full path on hover, FPS, frame time and frame count. **System Information** shows the
graphics API, GPU and CPU.

## Command-line options

| Option | Meaning |
| --- | --- |
| `--model NAME_OR_PATH` | Khronos sample-asset name, or a path to a `.gltf`/`.glb` (optional in the UI) |
| `--asset-dir PATH` | Directory used for model-name lookup |
| `--list-models` | List locally checked-out sample models and their paths |
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

## README image

The [README image](images/damaged-helmet.png) is direct PNG output from snr's path tracer,
with a studio floor and point light and no postprocessing. To reproduce it:

```sh
build/snr.exe --model DamagedHelmet --studio --width 960 --height 640 --spp 2048 --batch-size 128 --fov 32 --environment 0.2 --exposure 1.5 --validate --output output/readme-damaged-helmet.png
```

Other settings use their defaults, including seed 1 and a maximum of 12 bounces. The PNG was
rendered on Windows with an NVIDIA GeForce RTX 4070. To refresh the checked-in image, copy the
generated PNG to `docs/images/damaged-helmet.png`; keep the caption and command aligned with it.

The model comes from the pinned Khronos glTF-Sample-Assets submodule. Its
[upstream credits](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/c6a6bd13ab2b3c685c7903d03561b8a9392f38b8/Models/DamagedHelmet/README.md)
and [license record](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/c6a6bd13ab2b3c685c7903d03561b8a9392f38b8/Models/DamagedHelmet/LICENSE.md)
list:

- © 2018 ctxwing, rebuild and conversion to glTF —
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).
- © 2016 theblueturtle_, earlier version of the model —
  [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/legalcode).
