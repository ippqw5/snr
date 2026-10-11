#include "./renderer.h"

#include "../app/window.h"
#include "../gpu/device.h"
#include "../gpu/shader_library.h"
#include "../io/image_output.h"
#include "./rasterizer.h"
#include "./session.h"

#include <chrono>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

namespace snr
{
CameraSettings cameraFromScene(const Scene& scene)
{
    return {scene.eye, scene.target, scene.up, scene.fov};
}

namespace
{
// Mouse-driven camera for the interactive window: left drag orbits around the target, right drag
// pans both eye and target, and the wheel dollies along the view direction. Angles and distances are
// scaled by the view so a drag moves the same amount of screen space whatever the model's size.
void applyCameraInput(Window& window, CameraSettings& camera)
{
    const glm::vec2 cursor = window.cursorDelta();
    const glm::vec2 scroll = window.scrollDelta();

    glm::vec3 offset = camera.eye - camera.target;
    float     distance = glm::length(offset);
    if (distance <= 0)
        return;
    glm::vec3 forward = -offset / distance;
    glm::vec3 right = glm::normalize(glm::cross(forward, camera.up));
    glm::vec3 up = glm::cross(right, forward);

    if (window.leftButton() && cursor != glm::vec2(0))
    {
        constexpr float kOrbitDegreesPerPixel = 0.3f;
        // Yaw about the world up axis, then pitch about the camera's own right axis, so the target
        // stays put and the eye travels on a sphere around it.
        glm::mat4 yaw =
            glm::rotate(glm::mat4(1.0f), glm::radians(-cursor.x * kOrbitDegreesPerPixel), camera.up);
        offset = glm::vec3(yaw * glm::vec4(offset, 0.0f));
        glm::mat4 pitch =
            glm::rotate(glm::mat4(1.0f), glm::radians(-cursor.y * kOrbitDegreesPerPixel), right);
        offset = glm::vec3(pitch * glm::vec4(offset, 0.0f));
        distance = glm::length(offset);
        camera.eye = camera.target + offset;
    }

    if (window.rightButton() && cursor != glm::vec2(0))
    {
        constexpr float kPanFractionPerPixel = 0.0015f;
        glm::vec3       shift = (-cursor.x * right + cursor.y * up) * (kPanFractionPerPixel * distance);
        camera.eye += shift;
        camera.target += shift;
    }

    if (scroll.y != 0)
    {
        constexpr float kDollyFractionPerNotch = 0.1f;
        glm::vec3       shift = glm::normalize(camera.target - camera.eye) * (scroll.y * kDollyFractionPerNotch);
        camera.eye += shift;
        camera.target += shift;
    }
}

void reportSetup(std::chrono::steady_clock::time_point setupStart)
{
    std::printf(
        "Device/scene/shader setup: %.3f seconds\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStart).count()
    );
    std::fflush(stdout);
}

// The interactive preview: the raster pass fills the film, the present pass tone maps it into the
// acquired swapchain image, and the frame ends with a present. A resize reconfigures the surface and
// resizes the session's targets, so the two never disagree on extent; a minimized window has no
// drawable area and skips the frame.
void renderWindowed(const Scene& scene, const RenderOptions& options)
{
    auto           setupStart = std::chrono::steady_clock::now();
    DeviceContext  context(options.shaderDir, options.validation);
    RenderSettings settings(options.image, cameraFromScene(scene), options.integrator);
    settings.debugView = options.debugView;
    ShaderLibrary  shaders(context, "rasterizer.slang");

    Window        window("snr", settings.imageSize, context);
    RasterSession session(shaders, scene, settings);
    reportSetup(setupStart);

    CameraSettings camera = settings.camera;
    while (window.pump())
    {
        const ImageSize size = window.framebufferSize();
        if (size.width == 0 || size.height == 0)
            continue;

        if (size != session.settings().imageSize)
            session.resize(size);

        applyCameraInput(window, camera);
        session.setCamera(camera);

        auto image = window.acquireImage();
        if (!image)
            continue;

        session.render();
        session.present(image, options.display);
        window.present();
    }
}
} // namespace

// Both renderers are peers over the same scene snapshot, film and image output: they differ only in
// the shader module, the session type and how the frame is drawn, so the surrounding setup, timing
// and output stay in one place.
void render(const Scene& scene, const RenderOptions& options)
{
    // The window owns its own loop and never writes an image, so it leaves before the one-shot setup.
    if (options.windowed)
    {
        renderWindowed(scene, options);
        return;
    }

    auto            setupStart = std::chrono::steady_clock::now();
    DeviceContext   context(options.shaderDir, options.validation);
    RenderSettings  settings(options.image, cameraFromScene(scene), options.integrator);
    auto            start = std::chrono::steady_clock::now();
    RenderResult    result;

    settings.samplesPerPixel = options.spp;
    settings.batchSize = options.batchSize;
    settings.debugView = options.debugView;

    if (options.mode == RenderingMode::Raster)
    {
        ShaderLibrary shaders(context, "rasterizer.slang");
        RasterSession session(shaders, scene, settings);
        reportSetup(setupStart);

        session.render();
        result = session.readback(options.display);
    }
    else
    {
        ShaderLibrary shaders(context, "pathtracer.slang");
        PathSession session(shaders, scene, settings);
        reportSetup(setupStart);

        session.render();
        result = session.readback(options.display);
    }

    std::printf(
        "\nRender, tone mapping, and readback: %.3f seconds\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
    );
    saveImages(options.output, result);
}
} // namespace snr
