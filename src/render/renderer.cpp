

#include "./renderer.h"

#include "../gpu/device.h"
#include "../gpu/shader_library.h"
#include "../io/image_output.h"
#include "./session.h"

#include <chrono>
#include <cstdio>

namespace snr
{
CameraSettings cameraFromScene(const Scene& scene)
{
    return {scene.eye, scene.target, scene.up, scene.fov};
}

void render(const Scene& scene, const RenderOptions& options)
{
    auto           setupStart = std::chrono::steady_clock::now();
    DeviceContext  context(options.shaderDir, options.validation);
    ShaderLibrary  shaders(context, "pathtracer.slang");
    RenderSettings settings(options.image, cameraFromScene(scene), options.integrator);
    RenderSession  session(shaders, scene, settings);

    auto start = std::chrono::steady_clock::now();
    std::printf(
        "Device/scene/shader setup: %.3f seconds\n", std::chrono::duration<double>(start - setupStart).count()
    );
    std::fflush(stdout);

    session.renderTo(options.spp, options.batchSize);
    auto result = session.readback(options.display);
    std::printf("\nRender, tone mapping, and readback: %.3f seconds\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    saveImages(options.output, result);
}
} // namespace snr
