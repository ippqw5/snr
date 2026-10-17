# Renderer tests

Use these checks when changing renderer code or shaders. They cover shader execution, material
transport and interactive accumulation behavior. Build the renderer using the
[build instructions](../README.md#building), then run commands from the repository root.

## Shader smoke tests

C++ compilation alone does not compile the shader entry points: shaders are compiled at runtime.
After changing shared shaders, exercise both renderers. These commands require a supported GPU
and the sample-assets submodule:

```sh
build/snr.exe --model DamagedHelmet --studio --width 64 --height 64 --spp 8 --batch-size 4 --validate --output output/path-check.png
build/snr.exe --model DamagedHelmet --studio --raster --width 64 --height 64 --validate --output output/raster-check.png
```

Check that shader compilation and rendering complete without validation errors and that both
commands produce PNG and HDR images. Inspect the images for obvious rendering regressions.

## Transmission and volume integration tests

[test_transmission.py](test_transmission.py) generates small glTF fixtures and checks absorption,
nested media, IOR, total internal reflection, texture sampling, refraction displacement and energy
conservation using linear HDR output. It requires Python and a supported GPU, with no external
scene assets or third-party Python packages.

```sh
python tests/test_transmission.py --exe build/snr.exe
```

The script exercises Path tracing and selected Raster cases. A successful run prints
`All transmission/volume GPU integration checks passed.` Fixtures and rendered images are created
in a temporary directory and removed when the script exits.

## Manual GUI regression checks

After changing UI controls or accumulation handling, start the GUI with a model:

```sh
build/snr.exe --model DamagedHelmet --studio --window --validate
```

Exercise the [accumulation reset rules](../docs/architecture.md#what-invalidates-accumulated-samples):

1. Accumulate a path-traced image, pause it, switch to Raster and back without changing state;
   the retained sample count and image should be preserved.
2. While in Raster, change the camera or render resolution, then return to Path tracing;
   the old accumulation should be replaced, with one fresh batch even while paused.
3. Change exposure while paused; the display should change while the sample count is preserved.
4. Import another valid model; the replacement should render without the old path accumulation.
   Try an invalid import; the working scene and camera should remain usable.
