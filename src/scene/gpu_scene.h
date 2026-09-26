#pragma once

#include "../gpu/device.h"
#include "./scene.h"

#include <slang-rhi.h>
#include <slang-rhi/shader-cursor.h>

class GpuScene
{
public:
    GpuScene(DeviceContext& context, const Scene& scene);
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void     bind(const rhi::ShaderCursor& root) const;
    uint32_t lightCount() const
    {
        return m_lightCount;
    }

private:
    uint32_t                                 m_lightCount;
    rhi::ComPtr<rhi::IAccelerationStructure> m_blas, m_tlas;
    rhi::ComPtr<rhi::IBuffer>                m_triangles, m_materials, m_lights, m_texels;
};