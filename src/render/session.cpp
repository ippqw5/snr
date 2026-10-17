#include "./session.h"
#include "./dispatch.h"
#include "gpu/device.h"
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
// A swapchain image is commonly an sRGB format, and writing to one applies the encode in hardware, so
// the present shader must leave linear values rather than encoding twice.
bool isSrgbFormat(rhi::Format format)
{
    using rhi::Format;
    return format == Format::RGBA8UnormSrgb || format == Format::BGRA8UnormSrgb ||
           format == Format::BGRX8UnormSrgb;
}

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

Session::Session(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings, const UploadProgress& progress)
    : m_context(shaders.context()), m_shaders(shaders), m_settings(settings), m_environment(scene.environment),
      m_scene(std::make_shared<GpuScene>(m_context, scene, progress)), m_film(m_context, settings.imageSize),
      m_tonemap(shaders.computePipeline("tonemapMain"))
{
}

Session::Session(ShaderLibrary& shaders, const Session& source)
    : m_context(shaders.context()), m_shaders(shaders), m_settings(source.m_settings),
      m_environment(source.m_environment), m_scene(source.m_scene), m_film(m_context, m_settings.imageSize),
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

void Session::present(rhi::ITexture* target, const DisplaySettings& display)
{
    using namespace rhi;
    if (!m_presentShaders)
        m_presentShaders = std::make_unique<ShaderLibrary>(m_context, "present.slang");
    const Format format = target->getDesc().format;
    if (!m_present || m_presentFormat != format)
    {
        ColorTargetDesc colorTarget = {};
        colorTarget.format = format;

        RenderPipelineDesc desc = {};
        desc.targets = &colorTarget;
        desc.targetCount = 1;
        desc.primitiveTopology = PrimitiveTopology::TriangleList;
        desc.rasterizer.cullMode = CullMode::None;
        m_present = m_presentShaders->renderPipeline("presentVS", "presentFS", desc, std::to_string(int(format)));
        m_presentFormat = format;
    }

    const auto    targetSize = target->getDesc().size;
    PresentParams params = {};
    params.image = {m_settings.imageSize.width, m_settings.imageSize.height, targetSize.width, targetSize.height};
    params.config = {display.exposure, isSrgbFormat(format) ? 1.0f : 0.0f, 0, 0};

    auto encoder = m_context.createEncoder();
    encoder->globalBarrier();

    RenderPassColorAttachment color = {};
    color.view = target->getDefaultView();
    color.loadOp = LoadOp::Clear;
    color.storeOp = StoreOp::Store;

    RenderPassDesc passDesc = {};
    passDesc.colorAttachments = &color;
    passDesc.colorAttachmentCount = 1;

    auto* pass = encoder->beginRenderPass(passDesc);
    if (!pass)
        throw std::runtime_error("begin present pass failed");
    auto* root = pass->bindPipeline(m_present);
    if (!root)
        throw std::runtime_error("bind present pipeline failed");

    ShaderCursor cursor(root);
    bindConstants(cursor, "presentParams", &params, sizeof(params));
    checkRhi(cursor["film"].setBinding(m_film.accumulation()), "bind present film");

    RenderState state = {};
    state.viewports[0] = Viewport::fromSize(targetSize.width, targetSize.height);
    state.viewportCount = 1;
    state.scissorRects[0] =
        ScissorRect::fromSize(targetSize.width, targetSize.height);
    state.scissorRectCount = 1;
    pass->setRenderState(state);

    // One fullscreen triangle covers the whole target; the shader derives the pixel from SV_Position.
    DrawArguments arguments = {};
    arguments.vertexCount = 3;
    arguments.instanceCount = 1;
    pass->draw(arguments);
    pass->end();

    m_context.submit(encoder);
    m_context.checkValidation();
}

PathSession::PathSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings)
    : Session(shaders, scene, settings)
{
    m_trace = shaders.computePipeline("traceMain");
}

PathSession::PathSession(ShaderLibrary& shaders, const Session& source)
    : Session(shaders, source), m_trace(shaders.computePipeline("traceMain"))
{
}

void PathSession::setSampling(uint32_t targetSamples, uint32_t samplesPerFrame)
{
    if (!targetSamples || !samplesPerFrame)
        throw std::runtime_error("Sample target and batch size must be positive");
    if (targetSamples < samples())
        reset();
    m_settings.samplesPerPixel = targetSamples;
    m_settings.batchSize = samplesPerFrame;
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
    auto replacement = std::make_shared<GpuScene>(m_context, scene);
    m_scene = std::move(replacement);
    m_environment = scene.environment;
    reset();
}

bool PathSession::renderStep()
{
    const uint32_t target = m_settings.samplesPerPixel;
    if (target < samples())
        throw std::runtime_error("Reset the session before reducing the sample count");
    if (samples() == target)
        return false;
    Params params = makeParams(m_settings, m_environment, m_scene->lightCount());
    params.image.z = samples();
    params.image.w = std::min(m_settings.batchSize, target - samples());
    dispatchImage(m_context, m_trace, params, [&](const rhi::ShaderCursor& cursor) {
        m_scene->bind(cursor);
        checkRhi(cursor["film"].setBinding(m_film.accumulation()), "bind film");
    });
    m_film.addSamples(params.image.w);
    m_context.checkValidation();
    return true;
}

void PathSession::render()
{
    while (renderStep())
    {
        std::printf("\rSamples: %u/%u", samples(), m_settings.samplesPerPixel);
        std::fflush(stdout);
    }
}

} // namespace snr
