#include "./session.h"
#include "gpu/device.h"
#include "./dispatch.h"
#include "shaders/scene_io.h.slang"
#include "slang-rhi.h"
#include "slang-rhi/shader-cursor.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace snr
{
void bindConstants(const rhi::ShaderCursor& cursor, const char* name, const void* data, size_t size)
{
    rhi::ShaderCursor constants;

    checkRhi(cursor[name].getDereferenced(constants), "dereference paramter buffer");
    if (!constants.isValid() || constants.getTypeLayout()->getSize() != size)
    {
        throw std::runtime_error("Host/Slang paramter layout dismatch");
    }

    checkRhi(constants.setData(data, size), "bind parameters");
}

namespace
{
Params makeParams(const RenderSettings& settings, glm::vec3 environment, uint32_t lightCount)
{
    const auto& camera = settings.camera;
    Params      params = {};
    glm::vec3   forward = glm::normalize(camera.target - camera.eye);
    glm::vec3   right = glm::normalize(glm::cross(forward, camera.up));
    glm::vec3   up = glm::cross(right, forward);
    params.image = {settings.imageSize.width, settings.imageSize.height, 0, 0};
    params.integrator = {
        settings.integrator.maxBounces,
        settings.integrator.seed,
        lightCount,
        settings.integrator.nee ? 1u : 0u
    };

    params.eye = glm::vec4(camera.eye, glm::tan(glm::radians(camera.fov) * 0.5f));
    params.forward =
        glm::vec4(forward, float(settings.imageSize.width) / float(settings.imageSize.height));
    params.right = glm::vec4(right, 0);
    params.up = glm::vec4(up, 0);
    params.environment = glm::vec4(environment, 0);

    return params;
}
} // namespace

Session::Session(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings)
    : m_context(shaders.context()), m_shaders(shaders), m_settings(settings), m_environment(scene.environment),
      m_scene(std::make_unique<GpuScene>(m_context, scene)), m_film(m_context, settings.imageSize),
      m_tonemap(shaders.computePipeline("tonemapMain"))
{
}

void Session::resize(ImageSize size)
{
    if (size == m_settings.imageSize)
        return;

    m_settings.imageSize = size;
    m_film.resize(size);
}

void Session::setCamera(const CameraSettings& camera)
{
    if (camera == m_settings.camera)
        return;
    m_settings.camera = camera;
    reset();
}

RenderResult Session::readback(const DisplaySettings& display)
{
    if (samples() == 0)
    {
        throw std::runtime_error("cannot read an empty film");
    }

    // tonemapMain only reads the image extent and the exposure, so the rest of the block stays zero.
    Params params = {};
    params.image = {m_settings.imageSize.width, m_settings.imageSize.height, 0, 0};
    params.environment = glm::vec4(0, 0, 0, display.exposure);

    dispatchImage(m_context, m_tonemap, params, [&](const rhi::ShaderCursor& cursor) {
        checkRhi(cursor["film"].setBinding(m_film.accumulation()), "bind tonemap film");
        checkRhi(cursor["display"].setBinding(m_film.display()), "bind display buffer");
    });

    auto result = m_film.readback();
    m_context.checkValidation();
    return result;
}

PathSession::PathSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings)
    : Session(shaders, scene, settings)
{
    m_trace = shaders.computePipeline("traceMain");
}

void PathSession::setIntegrator(const IntegratorSettings& integrator)
{
    if (integrator == m_settings.integrator)
        return;
    m_settings.integrator = integrator;
    reset();
}

void PathSession::setEnvironment(glm::vec3 environment)
{
    if (environment == m_environment)
        return;

    m_environment = environment;
    reset();
}

void PathSession::setScene(const Scene& scene)
{
    auto replacement = std::make_unique<GpuScene>(m_context, scene);
    m_scene = std::move(replacement);
    m_environment = scene.environment;
    reset();
}

void PathSession::render()
{
    const uint32_t target = m_settings.samplesPerPixel;
    if (target < samples())
    {
        throw std::runtime_error("Reset the session before reducing the sample count");
    }

    Params params = makeParams(m_settings, m_environment, m_scene->lightCount());

    while (samples() < target)
    {
        params.image.z = samples();
        params.image.w = std::min(m_settings.batchSize, target - samples());
        dispatchImage(m_context, m_trace, params, [&](const rhi::ShaderCursor& cursor) {
            m_scene->bind(cursor);
            checkRhi(cursor["film"].setBinding(m_film.accumulation()), "bind film");
        });
        m_film.addSamples(params.image.w);
        std::printf("\rSamples: %u/%u", samples(), target);
        std::fflush(stdout);
    }
    m_context.checkValidation();
}
} // namespace snr
