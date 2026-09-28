#pragma once

#include "../gpu/device.h"
#include "./scene.h"

#include <slang-rhi/shader-cursor.h>

namespace snr
{
// An immutable GPU snapshot. CPU scene data is not retained after upload.
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
    uint32_t m_lightCount;
    // BLAS must remain alive with the TLAS references it.
    rhi::ComPtr<rhi::IAccelerationStructure> m_blas, m_tlas;
    rhi::ComPtr<rhi::IBuffer>                m_triangles, m_materials, m_lights, m_texels;
};
} // namespace snr
