#include "./rasterizer.h"

#include "./dispatch.h"
#include "shaders/scene_io.h.slang"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <slang-rhi/shader-cursor.h>
#include <stdexcept>
#include <vector>

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
    // Target 0 is the shaded image; target 1 is the instance under each pixel, written as an integer
    // so a picking or segmentation pass can read it back without decoding a colour.
    ColorTargetDesc targets[2] = {};
    targets[0].format = Format::RGBA32Float;
    targets[1].format = Format::R32Uint;

    RenderPipelineDesc desc = {};
    desc.targets = targets;
    desc.targetCount = 2;
    desc.primitiveTopology = PrimitiveTopology::TriangleList;
    desc.depthStencil.format = Format::D32Float;
    desc.depthStencil.depthTestEnable = true;
    desc.depthStencil.depthWriteEnable = true;
    desc.depthStencil.depthFunc = ComparisonFunc::Greater; // Reversed-Z
    // glTF models are commonly single-sided and the path tracer does not cull either, so the preview
    // draws both faces instead of guessing per material.
    desc.rasterizer.cullMode = CullMode::None;
    m_pipeline = shaders.renderPipeline("rasterVS", "rasterFS", desc);

    // The BLEND group draws after that one, over the depth it left. It cannot write depth, or a
    // nearer transparent surface would hide the ones behind it that still have to blend through, and
    // it blends source-over so the coverage the fragment shader kept decides how much shows.
    ColorTargetDesc blendTargets[2] = {targets[0], targets[1]};
    blendTargets[0].enableBlend = true;
    blendTargets[0].color.srcFactor = BlendFactor::SrcAlpha;
    blendTargets[0].color.dstFactor = BlendFactor::InvSrcAlpha;
    // The colour target's alpha doubles as the film's sample counter (see the readback check in
    // image_output.cpp), so it keeps the one the clear left instead of collecting coverage.
    blendTargets[0].alpha.srcFactor = BlendFactor::Zero;
    blendTargets[0].alpha.dstFactor = BlendFactor::One;

    RenderPipelineDesc blendDesc = desc;
    blendDesc.targets = blendTargets;
    blendDesc.depthStencil.depthWriteEnable = false;
    m_blendPipeline = shaders.renderPipeline("rasterVS", "rasterFS", blendDesc, "blend");

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

    TextureDesc objectIdDesc = colorDesc;
    objectIdDesc.format = Format::R32Uint;
    objectIdDesc.usage = TextureUsage::RenderTarget;
    ComPtr<ITexture> objectId;
    checkRhi(device->createTexture(objectIdDesc, nullptr, objectId.writeRef()), "create raster object-id target");
    ComPtr<ITextureView> objectIdView;
    checkRhi(objectId->createView({}, objectIdView.writeRef()), "create raster object-id view");

    m_color = color;
    m_colorView = colorView;
    m_depth = depth;
    m_depthView = depthView;
    m_objectId = objectId;
    m_objectIdView = objectIdView;
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

    // Object ids are one-based, so the zero a cleared pixel keeps means "nothing here".
    RenderPassColorAttachment objectId = {};
    objectId.view = m_objectIdView;
    objectId.loadOp = LoadOp::Clear;
    objectId.storeOp = StoreOp::Store;

    RenderPassColorAttachment colors[2] = {color, objectId};

    RenderPassDepthStencilAttachment depth = {};
    depth.view = m_depthView;
    depth.depthLoadOp = LoadOp::Clear;
    depth.depthStoreOp = StoreOp::DontCare;
    depth.depthClearValue = 0.0f; // Reversed-Z: zero is the far plane

    RenderPassDesc passDesc = {};
    passDesc.colorAttachments = colors;
    passDesc.colorAttachmentCount = 2;
    passDesc.depthStencilAttachment = &depth;

    auto* pass = encoder->beginRenderPass(passDesc);
    if (!pass)
        throw std::runtime_error("begin raster pass failed");

    RenderState state = {};
    state.viewports[0] = Viewport::fromSize(m_settings.imageSize.width, m_settings.imageSize.height);
    state.viewportCount = 1;
    state.scissorRects[0] = ScissorRect::fromSize(m_settings.imageSize.width, m_settings.imageSize.height);
    state.scissorRectCount = 1;

    // Binding a pipeline replaces the root object the cursor points into, so the constants and the
    // scene arrays are bound again for each pipeline. The viewport survives a bind but is cheap to
    // repeat, and leaving it here keeps the two groups' setup identical.
    auto bindGroup = [&](IRenderPipeline* pipeline) {
        auto* root = pass->bindPipeline(pipeline);
        if (!root)
            throw std::runtime_error("bind raster pipeline failed");
        ShaderCursor cursor(root);
        bindConstants(cursor, "rasterParams", &params, sizeof(params));
        m_scene->bindRaster(cursor);
        pass->setRenderState(state);
    };

    // One draw per instance, with no vertex or index buffer bound: the vertex shader pulls its corners
    // from the scene's index and position arrays, and startInstanceLocation carries the instance
    // index, which the vertex shader reads back through SV_StartInstanceLocation.
    const std::vector<InstanceDraw>& draws = m_scene->draws();
    auto                             drawInstance = [&](uint32_t index) {
        DrawArguments arguments = {};
        arguments.vertexCount = draws[index].triangleCount * 3;
        arguments.instanceCount = 1;
        arguments.startInstanceLocation = index;
        pass->draw(arguments);
    };

    // Geometry first, so the transparent group has depth to test against.
    bindGroup(m_pipeline);
    std::vector<uint32_t> transparent;
    for (uint32_t index = 0; index < uint32_t(draws.size()); ++index)
    {
        if (draws[index].blend)
            transparent.push_back(index);
        else
            drawInstance(index);
    }

    if (!transparent.empty())
    {
        // Back to front by instance origin: whatever is drawn later blends over what is already in the
        // colour target, so the farthest goes first. Sorting whole instances rather than triangles is
        // the approximation a preview can afford, and it is stable so equal distances keep scene
        // order.
        const glm::vec3 eye = m_settings.camera.eye;
        std::stable_sort(transparent.begin(), transparent.end(), [&](uint32_t a, uint32_t b) {
            const glm::vec3 offsetA = draws[a].origin - eye;
            const glm::vec3 offsetB = draws[b].origin - eye;
            return glm::dot(offsetA, offsetA) > glm::dot(offsetB, offsetB);
        });

        bindGroup(m_blendPipeline);
        for (uint32_t index : transparent)
            drawInstance(index);
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
