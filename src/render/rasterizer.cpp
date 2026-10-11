#include "./rasterizer.h"

#include "./dispatch.h"
#include "shaders/scene_io.h.slang"

#include <cmath>
#include <cstdio>
#include <slang-rhi/shader-cursor.h>
#include <stdexcept>

namespace snr
{
using namespace rhi;

namespace
{
// Reversed-Z: clip.z is the constant near plane and clip.w is the view depth, so the stored depth is
// near / viewZ. That keeps float precision even at great distances and needs no far plane, which
// matters because the preview does not know how far away the scene extends.
constexpr float kNear = 0.01f;

// A swapchain image is commonly an sRGB format, and writing to one applies the encode in hardware, so
// the present shader must leave linear values rather than encoding twice.
bool isSrgbFormat(Format format)
{
    return format == Format::RGBA8UnormSrgb || format == Format::BGRA8UnormSrgb ||
           format == Format::BGRX8UnormSrgb;
}

RasterParams makeRasterParams(const RenderSettings& settings, glm::vec3 environment, uint32_t lightCount)
{
    const auto& camera = settings.camera;
    glm::vec3   forward = glm::normalize(camera.target - camera.eye);
    glm::vec3   right = glm::normalize(glm::cross(forward, camera.up));
    glm::vec3   up = glm::cross(right, forward);
    float       tanHalf = std::tan(glm::radians(camera.fov) * 0.5f);
    float       aspect = float(settings.imageSize.width) / float(settings.imageSize.height);

    // The x and y rows reproduce the path tracer's ray basis, so the two renderers frame the model
    // identically. Slang's SPIR-V output already flips clip y for Vulkan, so the up row keeps the
    // basis positive and world up lands at the top of the image.
    RasterParams params = {};
    params.viewProjRows[0] =
        glm::vec4(right / (aspect * tanHalf), -glm::dot(right, camera.eye) / (aspect * tanHalf));
    params.viewProjRows[1] = glm::vec4(up / tanHalf, -glm::dot(up, camera.eye) / tanHalf);
    params.viewProjRows[2] = glm::vec4(0, 0, 0, kNear);
    params.viewProjRows[3] = glm::vec4(forward, -glm::dot(forward, camera.eye));
    params.eye = glm::vec4(camera.eye, tanHalf);
    params.environment = glm::vec4(environment, 0);
    params.counts = glm::uvec4(lightCount, 0, 0, 0);
    return params;
}
} // namespace

RasterSession::RasterSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings)
    : Session(shaders, scene, settings)
{
    ColorTargetDesc target = {};
    target.format = Format::RGBA32Float;

    RenderPipelineDesc desc = {};
    desc.targets = &target;
    desc.targetCount = 1;
    desc.primitiveTopology = PrimitiveTopology::TriangleList;
    desc.depthStencil.format = Format::D32Float;
    desc.depthStencil.depthTestEnable = true;
    desc.depthStencil.depthWriteEnable = true;
    desc.depthStencil.depthFunc = ComparisonFunc::Greater; // Reversed-Z
    // glTF models are commonly single-sided and the path tracer does not cull either, so the preview
    // draws both faces instead of guessing per material.
    desc.rasterizer.cullMode = CullMode::None;
    m_pipeline = shaders.renderPipeline("rasterVS", "rasterFS", desc);

    createTargets(settings.imageSize);
}

void RasterSession::createTargets(ImageSize size)
{
    auto* device = m_context.device();

    TextureDesc colorDesc = {};
    colorDesc.type = TextureType::Texture2D;
    colorDesc.size = {size.width, size.height, 1};
    colorDesc.format = Format::RGBA32Float;
    colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    colorDesc.defaultState = ResourceState::RenderTarget;
    ComPtr<ITexture> color;
    checkRhi(device->createTexture(colorDesc, nullptr, color.writeRef()), "create raster color target");
    ComPtr<ITextureView> colorView;
    checkRhi(color->createView({}, colorView.writeRef()), "create raster color view");

    TextureDesc depthDesc = colorDesc;
    depthDesc.format = Format::D32Float;
    depthDesc.usage = TextureUsage::DepthStencil;
    depthDesc.defaultState = ResourceState::DepthWrite;
    ComPtr<ITexture> depth;
    checkRhi(device->createTexture(depthDesc, nullptr, depth.writeRef()), "create raster depth target");
    ComPtr<ITextureView> depthView;
    checkRhi(depth->createView({}, depthView.writeRef()), "create raster depth view");

    m_color = color;
    m_colorView = colorView;
    m_depth = depth;
    m_depthView = depthView;
}

void RasterSession::resize(ImageSize size)
{
    if (size == m_settings.imageSize)
        return;

    Session::resize(size);
    createTargets(size);
}

void RasterSession::render()
{
    RasterParams params = makeRasterParams(m_settings, m_environment, m_scene->lightCount());

    m_film.reset();

    auto encoder = m_context.createEncoder();
    encoder->globalBarrier();

    RenderPassColorAttachment color = {};
    color.view = m_colorView;
    color.loadOp = LoadOp::Clear;
    color.storeOp = StoreOp::Store;
    // Pixels the geometry never covers show the environment, matching the path tracer's miss radiance
    // rather than a black backdrop.
    color.clearValue[0] = m_environment.x;
    color.clearValue[1] = m_environment.y;
    color.clearValue[2] = m_environment.z;
    color.clearValue[3] = 1.0f;

    RenderPassDepthStencilAttachment depth = {};
    depth.view = m_depthView;
    depth.depthLoadOp = LoadOp::Clear;
    depth.depthStoreOp = StoreOp::DontCare;
    depth.depthClearValue = 0.0f; // Reversed-Z: zero is the far plane

    RenderPassDesc passDesc = {};
    passDesc.colorAttachments = &color;
    passDesc.colorAttachmentCount = 1;
    passDesc.depthStencilAttachment = &depth;

    auto* pass = encoder->beginRenderPass(passDesc);
    if (!pass)
        throw std::runtime_error("begin raster pass failed");
    auto* root = pass->bindPipeline(m_pipeline);
    if (!root)
        throw std::runtime_error("bind raster pipeline failed");

    ShaderCursor cursor(root);
    bindConstants(cursor, "rasterParams", &params, sizeof(params));
    m_scene->bindRaster(cursor);

    RenderState state = {};
    state.viewports[0] = Viewport::fromSize(m_settings.imageSize.width, m_settings.imageSize.height);
    state.viewportCount = 1;
    state.scissorRects[0] = ScissorRect::fromSize(m_settings.imageSize.width, m_settings.imageSize.height);
    state.scissorRectCount = 1;
    pass->setRenderState(state);

    // One draw per instance, with no vertex or index buffer bound: the vertex shader pulls its corners
    // from the scene's index and position arrays, and startInstanceLocation carries the instance
    // index, which the vertex shader reads back through SV_StartInstanceLocation.
    const std::vector<InstanceDraw>& draws = m_scene->draws();
    for (uint32_t index = 0; index < uint32_t(draws.size()); ++index)
    {
        DrawArguments arguments = {};
        arguments.vertexCount = draws[index].triangleCount * 3;
        arguments.instanceCount = 1;
        arguments.startInstanceLocation = index;
        pass->draw(arguments);
    }
    pass->end();

    // The pass leaves linear radiance in the colour target; the film expects it in its accumulation
    // buffer, so copy it across as the first and only sample.
    encoder->copyTextureToBuffer(
        m_film.accumulation(),
        0,
        size_t(m_settings.imageSize.width) * m_settings.imageSize.height * sizeof(glm::vec4),
        size_t(m_settings.imageSize.width) * sizeof(glm::vec4),
        m_color,
        0,
        0,
        {0, 0, 0},
        {m_settings.imageSize.width, m_settings.imageSize.height, 1}
    );

    m_context.submitAndWait(encoder);
    m_film.addSamples(1);
    m_context.checkValidation();
}

void RasterSession::present(rhi::ITexture* target, const DisplaySettings& display)
{
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
        m_present = m_shaders.renderPipeline("presentVS", "presentFS", desc);
        m_presentFormat = format;
    }

    if (m_settings.imageSize.width != target->getDesc().size.width ||
        m_settings.imageSize.height != target->getDesc().size.height)
    {
        throw std::runtime_error("present target size does not match the session image size");
    }

    PresentParams params = {};
    params.image = {m_settings.imageSize.width, m_settings.imageSize.height, 0, 0};
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
    state.viewports[0] = Viewport::fromSize(m_settings.imageSize.width, m_settings.imageSize.height);
    state.viewportCount = 1;
    state.scissorRects[0] =
        ScissorRect::fromSize(m_settings.imageSize.width, m_settings.imageSize.height);
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
} // namespace snr
