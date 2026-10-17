# Presentation implementation

Read [presentation and file output](../architecture.md#presentation-and-file-output) for output
semantics and color encoding. This reference covers the display pipeline, fitting and window integration.

[Architecture](../architecture.md) · [Project README](../../README.md)

## Display pipeline

`Session::present()` loads present.slang lazily and caches a graphics pipeline for the swapchain format.
`PresentParams::image.xy` holds film dimensions and `.zw` output dimensions.
Bilinear sampling fits the linear film with preserved aspect ratio and black bars.
See [output encoding](../architecture.md#presentation-and-file-output) for how the target format affects the shader.

## Window integration

Window resize requests convert framebuffer pixels to GLFW screen coordinates before the next event
pump reconfigures the surface. Render resolution and window dimensions can therefore differ.

Dear ImGui's docking release uses `ImGuiPlatformIO` callbacks to manage each detached panel's GLFW
window and slang-rhi surface. Draw data uses the viewport's display origin and framebuffer scale.
For surface release order and minimized-window behavior, see
[window resources](../architecture.md#window-resources). For panel operations and controls, see [usage](../usage.md#interactive-controls).
