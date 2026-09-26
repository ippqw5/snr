#pragma once

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <slang-rhi.h>
#include <vector>

void checkRhi(rhi::Result result, const char* operation);

rhi::ComPtr<rhi::IBuffer> makeBuffer(
    rhi::IDevice*      device,
    size_t             size,
    size_t             stride,
    rhi::BufferUsage   usage,
    rhi::ResourceState state,
    const void*        data = nullptr
);

template <typename T>
rhi::ComPtr<rhi::IBuffer> upload(rhi::IDevice* device, const std::vector<T>& data)
{
    T zero = {};
    return makeBuffer(
        device,
        std::max(size_t(1), data.size() * sizeof(T)),
        sizeof(T),
        rhi::BufferUsage::ShaderResource,
        rhi::ResourceState::ShaderResource,
        data.empty() ? &zero : data.data()
    );
}

class DeviceContext
{
public:
    DeviceContext(const std::filesystem::path& shaderDir, bool validation = false);
    ~DeviceContext();
    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    rhi::IDevice* device() const
    {
        return m_device;
    }
    const std::filesystem::path& shaderDir() const
    {
        return m_shaderDir;
    }
    rhi::ComPtr<rhi::ICommandEncoder> createEncoder() const;
    void                              submitAndWait(rhi::ICommandEncoder* encoder) const;
    void                              checkValidation() const;

private:
    class DebugCallback : public rhi::IDebugCallback
    {
    public:
        std::atomic<unsigned> errors{0};
        void SLANG_MCALL      handleMessage(
            rhi::DebugMessageType   type,
            rhi::DebugMessageSource source,
            const char*             message
        ) noexcept override;
    };
    DebugCallback m_debugCallback;

    std::filesystem::path           m_shaderDir;
    rhi::ComPtr<rhi::IDevice>       m_device;
    rhi::ComPtr<rhi::ICommandQueue> m_queue;
};