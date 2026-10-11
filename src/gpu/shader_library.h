#pragma once

#include "./device.h"

#include <map>
#include <string>

class ShaderLibrary
{
public:
    ShaderLibrary(DeviceContext& context, const std::filesystem::path& modulePath);
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    rhi::IComputePipeline* computePipeline(const std::string& entryPoint);
    // Composes the vertex and fragment entry points into one program and creates the pipeline from
    // `desc` (whose program field is filled in here). Cached per entry-point pair and variant, so one
    // pair of entry points can back several pipeline states -- the raster preview's opaque and blended
    // passes share theirs.
    rhi::IRenderPipeline* renderPipeline(
        const std::string&             vertexEntryPoint,
        const std::string&             fragmentEntryPoint,
        const rhi::RenderPipelineDesc& desc,
        const std::string&             variant = {}
    );
    DeviceContext&         context() const
    {
        return m_context;
    }

private:
    DeviceContext&                                            m_context;
    rhi::ComPtr<slang::IModule>                               m_module;
    std::map<std::string, rhi::ComPtr<rhi::IComputePipeline>> m_computePipelines;
    std::map<std::string, rhi::ComPtr<rhi::IRenderPipeline>>  m_renderPipelines;
};