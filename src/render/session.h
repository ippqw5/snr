#pragma once

#include "../gpu/device.h"
#include "../gpu/shader_library.h"
#include "../scene/gpu_scene.h"
#include "../scene/scene.h"
#include "./film.h"
#include "./result.h"
#include "./settings.h"

#include <memory>

namespace snr
{
// The part every render session shares: a scene snapshot, the settings, the film that collects linear
// radiance, and the tone-map/readback path that turns a filled film into a result. A subclass owns its
// own pipelines and implements render(), so a caller can drive either renderer through this interface.
//
// The library's device context must outlive the session. Shader pipelines are retained after
// construction.
class Session
{
public:
    Session(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings);
    virtual ~Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const RenderSettings& settings() const
    {
        return m_settings;
    }
    uint32_t samples() const
    {
        return m_film.samples();
    }
    void reset()
    {
        m_film.reset();
    }
    virtual void resize(ImageSize size);
    void         setCamera(const CameraSettings& camera);

    // Draw the frame into the film. The path tracer accumulates its sample batches here; the raster
    // preview records its single pass.
    virtual void render() = 0;

    RenderResult readback(const DisplaySettings& display = {});

protected:
    DeviceContext&                     m_context;
    // Retained so a subclass can create a pipeline after construction, as the windowed present pass
    // does once it knows the swapchain's format.
    ShaderLibrary&                     m_shaders;
    RenderSettings                     m_settings;
    glm::vec3                          m_environment;
    std::unique_ptr<GpuScene>          m_scene;
    Film                               m_film;
    rhi::ComPtr<rhi::IComputePipeline> m_tonemap;
};

// Progressive path tracing: render() accumulates toward RenderSettings::samplesPerPixel, so a session
// can be extended by raising the target and rendering again.
class PathSession : public Session
{
public:
    PathSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings);

    void setIntegrator(const IntegratorSettings& integrator);
    void setEnvironment(glm::vec3 environment);
    void setScene(const Scene& scene);

    void render() override;

private:
    rhi::ComPtr<rhi::IComputePipeline> m_trace;
};
} // namespace snr
