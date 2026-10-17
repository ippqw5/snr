#include "./renderer.h"

#include "../app/preview_ui.h"
#include "../app/window.h"
#include "../gpu/device.h"
#include "../gpu/shader_library.h"
#include "../io/image_output.h"
#include "./rasterizer.h"
#include "./session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <thread>

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
        // Zoom about the orbit anchor: only the eye moves, so the next left drag still
        // rotates around the same target. A positive distance prevents crossing the anchor.
        const float currentDistance = glm::length(camera.eye - camera.target);
        const float newDistance = std::clamp(
            currentDistance * std::exp(-scroll.y * kDollyFractionPerNotch), 0.02f, 1e10f
        );
        camera.eye = camera.target + (camera.eye - camera.target) * (newDistance / currentDistance);
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
    constexpr std::array<uint32_t, 5> views = {
        rasterDebugShaded, rasterDebugDepth, rasterDebugObjectId, rasterDebugBaseColor, rasterDebugNormal
    };

    auto           setupStart = std::chrono::steady_clock::now();
    DeviceContext  context(options.shaderDir, options.validation);
    RenderSettings settings(options.image, cameraFromScene(scene), options.integrator);
    ShaderLibrary  shaders(context, "rasterizer.slang");

    Window                         window("snr gui", settings.imageSize, context);
    PreviewUi                      ui(context, window, options.modelPath.empty() ? options.assetDir : options.modelPath.parent_path());
    std::unique_ptr<ShaderLibrary> pathShaders;
    std::unique_ptr<PathSession>   pathSession;
    std::unique_ptr<RasterSession> session;
    PreviewControls                controls;
    controls.resolution = options.image;
    controls.targetSamples = options.spp;
    controls.integrator = options.integrator;
    controls.display = options.display;
    controls.studio = options.studio;
    if (!scene.instances.empty())
        session = std::make_unique<RasterSession>(shaders, scene, settings);
    if (!options.modelPath.empty())
        ui.setModel(options.modelPath, scene.name);
    reportSetup(setupStart);

    CameraSettings        camera = settings.camera;
    uint32_t              viewIndex = 0;
    std::future<Scene>    pendingImport;
    std::filesystem::path pendingPath;
    auto                  lastLoadingFrame = std::chrono::steady_clock::time_point{};
    std::string           lastLoadingStage;
    // GPU creation stays on this thread. Repaint the loading UI at safe points between operations.
    auto uploadProgress = [&](float fraction, const char* stage) {
        ui.setLoading(true, fraction, stage);
        const auto now = std::chrono::steady_clock::now();
        if (lastLoadingStage == stage && now - lastLoadingFrame < std::chrono::milliseconds(33))
            return;
        lastLoadingFrame = now;
        lastLoadingStage = stage;
        if (!window.pump(ui.capturesKeyboard()))
            throw std::runtime_error("Import cancelled: window closed");
        ui.update(viewIndex, camera, controls, pathSession ? pathSession->samples() : 0);
        if (auto image = window.acquireImage())
        {
            ui.draw(image, true);
            window.present();
        }
        else
            ui.drawPlatformWindows();
    };
    while (window.pump(ui.capturesKeyboard()))
    {
        if (auto key = window.numberKeyPressed(); controls.mode == RenderingMode::Raster && !ui.capturesKeyboard() && key && *key >= 1 && *key <= views.size())
            viewIndex = *key - 1;
        ImageSize size = window.framebufferSize();

        if (pendingImport.valid() && pendingImport.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                Scene           imported = pendingImport.get();
                const ImageSize drawable = controls.followWindow && size.width && size.height ? size : controls.resolution;
                RenderSettings  replacementSettings(drawable, cameraFromScene(imported), options.integrator);
                // Build the replacement before releasing the working scene, so failed imports leave
                // the existing preview and camera intact.
                auto replacement = std::make_unique<RasterSession>(shaders, imported, replacementSettings, uploadProgress);
                context.queue()->waitOnHost();
                pathSession.reset();
                session = std::move(replacement);
                camera = replacementSettings.camera;
                ui.setModel(pendingPath, imported.name);
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr, "Import failed: %s\n", error.what());
                ui.setError(error.what());
            }
            ui.setLoading(false);
            // Progress callbacks process resize events while the replacement is being built.
            size = window.framebufferSize();
        }
        if (controls.followWindow && size.width && size.height)
            controls.resolution = size;
        ui.update(viewIndex, camera, controls, pathSession ? pathSession->samples() : 0);
        if (auto path = ui.takeModel(); path && !pendingImport.valid())
        {
            pendingPath = *path;
            ui.setLoading(true);
            try
            {
                pendingImport = std::async(std::launch::async, [path = *path, studio = controls.studio] {
                    Scene imported;
                    if (!imported.loadGltf(path))
                    {
                        std::string errors = "Cannot import this scene";
                        for (const auto& error : imported.errors)
                            errors += "\n" + error;
                        throw std::runtime_error(errors);
                    }
                    for (const auto& warning : imported.warnings)
                        std::fprintf(stderr, "Warning: %s\n", warning.c_str());
                    if (studio)
                        addStudio(imported);
                    return imported;
                });
            }
            catch (const std::exception& error)
            {
                ui.setLoading(false);
                ui.setError(error.what());
            }
        }
        if (size.width && size.height && !ui.capturesMouse())
            applyCameraInput(window, camera);
        if (controls.mode == RenderingMode::PathTracing && session && !pathSession)
        {
            try
            {
                if (!pathShaders)
                    pathShaders = std::make_unique<ShaderLibrary>(context, "pathtracer.slang");
                pathSession = std::make_unique<PathSession>(*pathShaders, *session);
            }
            catch (const std::exception& error)
            {
                controls.mode = RenderingMode::Raster;
                ui.setError(error.what());
            }
        }
        Session* active = controls.mode == RenderingMode::PathTracing
                              ? static_cast<Session*>(pathSession.get())
                              : session.get();
        if (active)
        {
            const ImageSize resolution = controls.followWindow && size.width && size.height ? size : controls.resolution;
            if (resolution != active->settings().imageSize)
                active->resize(resolution);
            active->setCamera(camera);
            if (controls.mode == RenderingMode::PathTracing)
            {
                pathSession->setIntegrator(controls.integrator);
                pathSession->setSampling(controls.targetSamples, controls.samplesPerFrame);
                if (controls.restart)
                    pathSession->reset();
            }
            else
                session->setDebugView(views[viewIndex]);
        }
        controls.restart = false;

        auto image = window.acquireImage();
        if (!image)
        {
            ui.drawPlatformWindows();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (active)
        {
            if (controls.mode == RenderingMode::PathTracing)
            {
                // Even when paused, draw a fresh first batch after camera/scene changes.
                if (!controls.paused || pathSession->samples() == 0)
                    pathSession->renderStep();
            }
            else
                session->render();
            active->present(image, controls.display);
        }
        ui.draw(image, !active);
        window.present();
    }
    context.queue()->waitOnHost();
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

    auto           setupStart = std::chrono::steady_clock::now();
    DeviceContext  context(options.shaderDir, options.validation);
    RenderSettings settings(options.image, cameraFromScene(scene), options.integrator);
    auto           start = std::chrono::steady_clock::now();
    RenderResult   result;

    settings.samplesPerPixel = options.spp;
    settings.batchSize = options.batchSize;

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
        PathSession   session(shaders, scene, settings);
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
