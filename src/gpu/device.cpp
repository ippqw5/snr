#include "./device.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

using namespace rhi;

void checkRhi(rhi::Result result, const char* operation)
{
    if (SLANG_FAILED(result))
    {
        throw std::runtime_error(
            std::string(operation) + "failed (" + std::to_string(result) + ")"
        );
    }
}

ComPtr<IBuffer> makeBuffer(
    IDevice*      device,
    size_t        size,
    size_t        stride,
    BufferUsage   usage,
    ResourceState state,
    const void*   data
)
{
    BufferDesc desc;
    desc.size = size;
    desc.elementSize = stride;
    desc.usage = usage;
    desc.defaultState = state;

    ComPtr<IBuffer> buffer;
    checkRhi(device->createBuffer(desc, data, buffer.writeRef()), "createBuffer");
    return buffer;
}

void DeviceContext::DebugCallback::handleMessage(
    DebugMessageType type,
    DebugMessageSource,
    const char* message
) noexcept
{
    if (type == DebugMessageType::Info)
    {
        return;
    }
    if (type == DebugMessageType::Error)
    {
        ++errors;
        std::fprintf(stderr, "[RHI]: %s\n", message);
    }
}

DeviceContext::DeviceContext(const std::filesystem::path& shaderDir, bool validation)
    : m_shaderDir(std::filesystem::canonical(shaderDir))
{
    auto* api = getRHI();
    if (!api)
    {
        throw std::runtime_error("Failed to initialize slang-rhi");
    }

    if (validation)
    {
        api->enableDebugLayers();
    }

    std::string path = m_shaderDir.string();
    const char* searchPaths[] = {path.c_str()};
    DeviceDesc  desc;
    desc.deviceType = DeviceType::Vulkan;
    desc.debugCallback = &m_debugCallback;
    desc.enableValidation = validation;
    desc.slang.searchPaths = searchPaths;
    desc.slang.searchPathCount = 1;
    desc.slang.targetProfile = "spirv_1_5";
    desc.slang.optimizationLevel = SlangOptimizationLevel::SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
    checkRhi(api->createDevice(desc, m_device.writeRef()), "create Vulkan device");

    if (!m_device->hasFeature(Feature::RayQuery))
    {
        throw std::runtime_error("Vulkan device does not support hardware ray queries");
    }

    m_queue = m_device->getQueue(QueueType::Graphics);
    if (!m_queue)
    {
        throw std::runtime_error("Could not create Vulkan graphics queue");
    }

    std::printf("GPU: %s\n", m_device->getInfo().adapterName);
    std::fflush(stdout);
}

DeviceContext::~DeviceContext()
{
    if (m_queue)
        m_queue->waitOnHost();
}

ComPtr<ICommandEncoder> DeviceContext::createEncoder() const
{
    auto encoder = m_queue->createCommandEncoder();
    if (!encoder)
    {
        throw std::runtime_error("Failed to create command encoder");
    }
    return encoder;
}

void DeviceContext::submitAndWait(rhi::ICommandEncoder* encoder) const
{
    ComPtr<ICommandBuffer> commands;
    checkRhi(encoder->finish(commands.writeRef()), "finish commands");
    checkRhi(m_queue->submit(commands), "submit commands");
    checkRhi(m_queue->waitOnHost(), "wait gfx idle");
}

void DeviceContext::checkValidation() const
{
    if (m_debugCallback.errors > 0)
    {
        throw std::runtime_error("RHI validation errors detected");
    }
}