#pragma once

#include "../gpu/device.h"
#include "./scene.h"

#include <slang-rhi/shader-cursor.h>

#include <vector>

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
    void buildAccelerationStructures(DeviceContext& context, const Scene& scene);

    uint32_t m_lightCount = 0;

    // The acceleration structures read these buffers, so they outlive the build.
    rhi::ComPtr<rhi::IBuffer> m_vertices, m_instanceData;
    // One bottom-level structure per primitive; the top-level structure references them all.
    std::vector<rhi::ComPtr<rhi::IAccelerationStructure>> m_blas;
    rhi::ComPtr<rhi::IAccelerationStructure>              m_tlas;

    rhi::ComPtr<rhi::IBuffer> m_triangles, m_primitives, m_instances, m_materials, m_lights, m_texels;
};
} // namespace snr