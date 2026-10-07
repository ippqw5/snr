#include "./shader_library.h"

#include <filesystem>
#include <slang-rhi.h>
#include <slang.h>
#include <stdexcept>

using namespace rhi;

static void printDiagnostics(slang::IBlob* blob)
{
    if (blob && blob->getBufferSize())
    {
        std::fprintf(stderr, "%s\n", static_cast<const char*>(blob->getBufferPointer()));
    }
}

ShaderLibrary::ShaderLibrary(DeviceContext& context, const std::filesystem::path& modulePath)
    : m_context(context)
{
    auto path = context.shaderDir() / modulePath;
    if (!std::filesystem::is_regular_file(path))
    {
        throw std::runtime_error("Missing shader: " + path.string());
    }
    path = std::filesystem::canonical(path);

    std::printf("Shader module: %s\n", path.string().c_str());
    std::fflush(stdout);

    ComPtr<slang::IBlob> diagnostics;
    m_module = context.device()->getSlangSession()->loadModule(
        path.string().c_str(),
        diagnostics.writeRef()
    );

    printDiagnostics(diagnostics);
    if (!m_module)
    {
        throw std::runtime_error("Slang shader module compilation failed: " + path.string());
    }
}

IComputePipeline* ShaderLibrary::computePipeline(const std::string& entryPoint)
{
    auto found = m_computePipelines.find(entryPoint);
    if (found != m_computePipelines.end())
    {
        return found->second;
    }

    auto*                      device = m_context.device();
    auto                       session = device->getSlangSession();
    ComPtr<slang::IEntryPoint> entry;
    checkRhi(
        m_module->findEntryPointByName(entryPoint.c_str(), entry.writeRef()),
        "find shader entry point"
    );

    slang::IComponentType*        components[] = {m_module, entry};
    ComPtr<slang::IComponentType> composite, linked;
    ComPtr<slang::IBlob>          diagnostics;
    auto                          result = session->createCompositeComponentType(
        components,
        2,
        composite.writeRef(),
        diagnostics.writeRef()
    );
    printDiagnostics(diagnostics);
    checkRhi(result, "compose shader program");

    result = composite->link(linked.writeRef(), diagnostics.writeRef());
    printDiagnostics(diagnostics);
    checkRhi(result, "link shader program");

    ShaderProgramDesc programDesc = {};
    programDesc.slangGlobalScope = linked;
    ComPtr<IShaderProgram> program;
    checkRhi(device->createShaderProgram(programDesc, program.writeRef()), "create shader program");

    ComputePipelineDesc pipelineDesc = {};
    pipelineDesc.program = program;
    ComPtr<IComputePipeline> pipeline;
    checkRhi(
        device->createComputePipeline(pipelineDesc, pipeline.writeRef()),
        "create compute pipeline"
    );

    return m_computePipelines.emplace(entryPoint, pipeline).first->second;
}