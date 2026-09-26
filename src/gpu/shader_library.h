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
    DeviceContext&         context() const
    {
        return m_context;
    }

private:
    DeviceContext&                                            m_context;
    rhi::ComPtr<slang::IModule>                               m_module;
    std::map<std::string, rhi::ComPtr<rhi::IComputePipeline>> m_computePipelines;
};