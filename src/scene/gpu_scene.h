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

    // The acceleration structures read the position and index buffers, so they outlive the build.
    // The shader reads the same two buffers, so the geometry is uploaded exactly once.
    rhi::ComPtr<rhi::IBuffer> m_positions, m_normals, m_baseColorUvs, m_metallicRoughnessUvs, m_indices;
    rhi::ComPtr<rhi::IBuffer> m_normalUvs, m_occlusionUvs, m_emissiveUvs;
    rhi::ComPtr<rhi::IBuffer> m_instanceData;
    // One bottom-level structure per primitive; the top-level structure references them all.
    std::vector<rhi::ComPtr<rhi::IAccelerationStructure>> m_blas;
    rhi::ComPtr<rhi::IAccelerationStructure>              m_tlas;

    rhi::ComPtr<rhi::IBuffer> m_primitives, m_instances, m_materials, m_lights, m_triangleLights, m_lightCdf;

    // The bindless texture and sampler heaps. Each texture view owns the descriptor handle it hands
    // out, so the views and samplers must outlive every binding that references them.
    std::vector<rhi::ComPtr<rhi::ITexture>>     m_textures;
    std::vector<rhi::ComPtr<rhi::ITextureView>> m_textureViews;
    std::vector<rhi::ComPtr<rhi::ISampler>>     m_samplers;
    std::vector<rhi::DescriptorHandle>          m_textureHandles;
    std::vector<rhi::DescriptorHandle>          m_samplerHandles;
};
} // namespace snr