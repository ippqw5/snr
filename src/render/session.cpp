#include "./session.h"
#include "gpu/device.h"
#include "slang-rhi.h"
#include "slang-rhi/shader-cursor.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace snr
{
struct Params
{
    glm::uvec4 image, integrator;
    glm::vec4  eye, forward, right, up, environment;
};
static_assert(sizeof(Params) == 112);

void bindParams(const rhi::ShaderCursor& cursor, const Params& params)
{
    rhi::ShaderCursor constants;

    checkRhi(cursor["params"].getDereferenced(constants), "dereference paramter buffer");
    if (!constants.isValid() || constants.getTypeLayout()->getSize() != sizeof(params))
    {
        throw std::runtime_error("Host/Slang paramter layout dismatch");
    }

    checkRhi(constants.setData(&params, sizeof(params)), "bind parameters");
}

template <typename BindResources>
void dispatchImage(
    DeviceContext&         context,
    rhi::IComputePipeline* pipeline,
    const Params&          params,
    BindResources          bindResources
)
{
    auto encoder = context.createEncoder();
    encoder->globalBarrier();
    auto* pass = encoder->beginComputePass();
    if (!pass)
        throw std::runtime_error("begin compute pass failed");
    auto* root = pass->bindPipeline(pipeline);
    if (!root)
        throw std::runtime_error("bind compute pipeline failed");
    rhi::ShaderCursor cursor(root);
    bindParams(cursor, params);
    bindResources(cursor);
    pass->dispatchCompute((params.image.x + 7) / 8, (params.image.y + 7) / 8, 1);
    pass->end();
    encoder->globalBarrier();
    context.submitAndWait(encoder);
}

static Params makeParams(
    const RenderSettings&  settings,
    glm::vec3              environment,
    uint32_t               lightCount,
    const DisplaySettings& display = {}
)
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
    params.environment = glm::vec4(environment, display.exposure);

    return params;
}

RenderSession::RenderSession(
    ShaderLibrary&        shaders,
    const Scene&          scene,
    const RenderSettings& settings
)
    : m_context(shaders.context()), m_settings(settings), m_environment(scene.environment), m_film(m_context, settings.imageSize)
{
    m_scene = std::make_unique<GpuScene>(m_context, scene);
    m_trace = shaders.computePipeline("traceMain");
    m_tonemap = shaders.computePipeline("tonemapMain");
}

void RenderSession::resize(ImageSize size)
{
    m_film.resize(size);
    m_settings.imageSize = size;
}

void RenderSession::setCamera(const CameraSettings& camera)
{
    if (camera == m_settings.camera)
        return;
    m_settings.camera = camera;
    reset();
}

void RenderSession::setIntegrator(const IntegratorSettings& integrator)
{
    if (integrator == m_settings.integrator)
        return;
    m_settings.integrator = integrator;
    reset();
}

void RenderSession::setEnvironment(glm::vec3 environment)
{
    if (environment == m_environment)
        return;

    m_environment = environment;
    reset();
}

void RenderSession::setScene(const Scene& scene)
{
    auto replacement = std::make_unique<GpuScene>(m_context, scene);
    m_scene = std::move(replacement);
    m_environment = scene.environment;
    reset();
}

void RenderSession::renderTo(uint32_t targetSamples, uint32_t batchSize)
{
    if (targetSamples < samples())
    {
        throw std::runtime_error("Reset the session before reducing the sample count");
    }

    Params params = makeParams(m_settings, m_environment, m_scene->lightCount());

    while (samples() < targetSamples)
    {
        params.image.z = samples();
        params.image.w = std::min(batchSize, targetSamples - samples());
        dispatchImage(m_context, m_trace, params, [&](const rhi::ShaderCursor& cursor) {
            m_scene->bind(cursor);
            checkRhi(cursor["film"].setBinding(m_film.accumulation()), "bind film");
        });
        m_film.addSamples(params.image.w);
        std::printf("\rSamples: %u/%u", samples(), targetSamples);
        std::fflush(stdout);
    }
    m_context.checkValidation();
}

RenderResult RenderSession::readback(const DisplaySettings& display)
{
    if (samples() == 0)
    {
        throw std::runtime_error("cannot read an empty film");
    }

    Params params = makeParams(m_settings, m_environment, m_scene->lightCount(), display);

    dispatchImage(m_context, m_tonemap, params, [&](const rhi::ShaderCursor& cursor) {
        checkRhi(cursor["film"].setBinding(m_film.accumulation()), "bind tonemap film");
        checkRhi(cursor["display"].setBinding(m_film.display()), "bind display buffer");
    });

    auto result = m_film.readback();
    m_context.checkValidation();
    return result;
}
} // namespace snr
