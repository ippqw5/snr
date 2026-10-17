#pragma once

#include "./session.h"

namespace snr
{
// A forward, single-pass raster preview, a peer of PathSession over the same scene snapshot, film and
// image output. Shading happens once per pixel in the fragment shader; there is no G-buffer and no
// second pass.
//
// OPAQUE uses forced early depth tests to avoid shading hidden surfaces. MASK uses normal depth
// writes so discarded texels do not occlude later draws. BLEND draws last, sorted back to front,
// blending colour without writing depth. All three pipelines share shading in one render pass.
//
// The colour target is offscreen rather than a swapchain image, so a window can replace it later
// without changing the pass. Whatever the fragment shader writes is copied into the film's
// accumulation buffer with sample count 1, after which the shared tonemap and image-output path runs
// exactly as it does for the path tracer. Alongside the colour the pass writes an object-id target
// (the instance under each pixel) for picking and segmentation.
class RasterSession : public Session
{
public:
    RasterSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings, const UploadProgress& progress = {});

    void resize(ImageSize size) override;
    void render() override;

    void setDebugView(uint32_t view)
    {
        m_settings.debugView = view;
    }

private:
    void createTargets(ImageSize size);

    rhi::ComPtr<rhi::IRenderPipeline> m_pipeline, m_opaquePipeline, m_blendPipeline;
    rhi::ComPtr<rhi::ITexture>        m_color, m_depth, m_objectId;
    rhi::ComPtr<rhi::ITextureView>    m_colorView, m_depthView, m_objectIdView;
};
} // namespace snr
