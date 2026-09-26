#pragma once

#include "../gpu/device.h"
#include "../gpu/shader_library.h"
#include "../scene/gpu_scene.h"
#include "../scene/scene.h"
#include "./film.h"
#include "./result.h"
#include "./settings.h"

#include <memory>

class RenderSession
{
public:
    RenderSession(ShaderLibrary& shaders, const Scene& scene, const RenderSettings& settings);
    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

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
    void resize(ImageSize size);
    void setCamera(const CameraSettings& camera);
    void setIntegrator(const IntegratorSettings& integrator);
    void setEnvironment(glm::vec3 environment);
    void setScene(const Scene& scene);

    void         renderTo(uint32_t targetSamples, uint32_t batchSize = 16);
    RenderResult readback(const DisplaySettings& display = {});

private:
    DeviceContext&                     m_context;
    RenderSettings                     m_settings;
    glm::vec3                          m_environment;
    std::unique_ptr<GpuScene>          m_scene;
    Film                               m_film;
    rhi::ComPtr<rhi::IComputePipeline> m_trace, m_tonemap;
};