#pragma once

#include "./session.h"

namespace snr
{
// A forward, single-pass raster preview, a peer of PathSession over the same scene snapshot, film and
// image output. Shading happens once per pixel in the fragment shader; there is no G-buffer and no
// second pass.
//
// Two pipelines share the one fragment shader. The first draws opaque and MASK instances with depth
// writes on; the second draws glTF BLEND instances after them, sorted back to front, blending over the
// colour rather than writing it. Both are recorded into a single render pass.
//
// The colour target is offscreen rather than a swapchain image, so a window can replace it later
// without changing the pass. Whatever the fragment shader writes is copied into the film's
// accumulation buffer with sample count 1, after which the shared tonemap and image-output path runs
// exactly as it does for the path tracer. Alongside the colour the pass writes an object-id target
// (the instance under each pixel) for picking and segmentation.
class RasterSession : public Session
{
public:
    RasterSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings);

    void resize(ImageSize size) override;
    void render() override;

    void setDebugView(uint32_t view)
    {
        m_settings.debugView = view;
    }

    // Tone maps the filled film straight into a swapchain image and submits without waiting, so the
    // caller can present. The present pipeline is created the first time a target format is seen, so
    // the window's surface — not this class — decides the swapchain format.
    void present(rhi::ITexture* target, const DisplaySettings& display);

private:
    void createTargets(ImageSize size);

    rhi::ComPtr<rhi::IRenderPipeline> m_pipeline, m_blendPipeline;
    rhi::ComPtr<rhi::IRenderPipeline> m_present;
    rhi::Format                       m_presentFormat = rhi::Format::Undefined;
    rhi::ComPtr<rhi::ITexture>        m_color, m_depth, m_objectId;
    rhi::ComPtr<rhi::ITextureView>    m_colorView, m_depthView, m_objectIdView;
};
} // namespace snr
