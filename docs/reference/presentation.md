# Presentation

Presentation connects a session's Film to a window. Read the architecture's
[output model](../architecture.md#presentation-and-file-output) for the relationship between
window display, CPU readback and file export, including their color-encoding differences.

## Display boundary

Rendering produces a linear Film at the chosen render resolution. Presentation reads that Film,
fits it into the drawable area with preserved aspect ratio, and applies the display transform.
This separates rendering resolution and accumulated samples from window dimensions and exposure.

The display pipeline depends on the swapchain format. Its encoding step complements the target
format's hardware conversion. The shared tone-mapping function gives window display and file
output a common exposure and tone-map definition.

## Window integration

Window dimensions, framebuffer pixels and Film dimensions are distinct coordinate spaces.
Window requests account for framebuffer scaling, while presentation handles image fitting.

Dear ImGui manages detached panels through platform callbacks. Each panel has a GLFW window and
a slang-rhi surface; draw data accounts for that viewport's origin and scale. The architecture
describes their [lifetime and shutdown order](../architecture.md#window-resources).
Panel operations are in [Usage](../usage.md#interactive-controls).

## Code entry points

| Source | What to look for |
| --- | --- |
| [session.cpp](../../src/render/session.cpp) | Display submission and format-dependent pipelines |
| [present.slang](../../shaders/present.slang) | Film sampling, image fitting and display encoding |
| [tonemap.h.slang](../../shaders/tonemap.h.slang) | Shared exposure and tone mapping |
| [window.cpp](../../src/app/window.cpp), [preview_ui.cpp](../../src/app/preview_ui.cpp) | Framebuffer scaling and detached viewports |
| [image_output.cpp](../../src/io/image_output.cpp) | CPU image export |
