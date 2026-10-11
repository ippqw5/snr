#include "./shader_library.h"

#include <filesystem>
#include <slang-rhi.h>
#include <slang.h>
#include <stdexcept>
#include <vector>

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

// Composes the module with the given entry points and links them into one program. Both pipeline
// kinds need this, and Slang requires every entry point to be handed in up front so they share one
// global scope.
static ComPtr<IShaderProgram> composeProgram(
    DeviceContext&                     context,
    slang::IModule*                    module,
    const std::vector<std::string>&    entryPointNames
)
{
    auto*                      device = context.device();
    auto                       session = device->getSlangSession();
    std::vector<ComPtr<slang::IComponentType>> components;
    components.push_back(ComPtr<slang::IComponentType>(module));
    for (const std::string& name : entryPointNames)
    {
        ComPtr<slang::IEntryPoint> entry;
        checkRhi(module->findEntryPointByName(name.c_str(), entry.writeRef()), "find shader entry point");
        components.push_back(ComPtr<slang::IComponentType>(entry.get()));
    }

    std::vector<slang::IComponentType*> rawComponents;
    rawComponents.reserve(components.size());
    for (auto& component : components)
        rawComponents.push_back(component.get());

    ComPtr<slang::IComponentType> composite, linked;
    ComPtr<slang::IBlob>          diagnostics;
    auto                          result = session->createCompositeComponentType(
        rawComponents.data(),
        rawComponents.size(),
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
    return program;
}

IComputePipeline* ShaderLibrary::computePipeline(const std::string& entryPoint)
{
    auto found = m_computePipelines.find(entryPoint);
    if (found != m_computePipelines.end())
    {
        return found->second;
    }

    ComPtr<IShaderProgram> program = composeProgram(m_context, m_module, {entryPoint});

    ComputePipelineDesc pipelineDesc = {};
    pipelineDesc.program = program;
    ComPtr<IComputePipeline> pipeline;
    checkRhi(
        m_context.device()->createComputePipeline(pipelineDesc, pipeline.writeRef()),
        "create compute pipeline"
    );

    return m_computePipelines.emplace(entryPoint, pipeline).first->second;
}

IRenderPipeline* ShaderLibrary::renderPipeline(
    const std::string&             vertexEntryPoint,
    const std::string&             fragmentEntryPoint,
    const rhi::RenderPipelineDesc& desc,
    const std::string&             variant
)
{
    std::string key = vertexEntryPoint + "|" + fragmentEntryPoint;
    if (!variant.empty())
        key += "|" + variant;
    auto found = m_renderPipelines.find(key);
    if (found != m_renderPipelines.end())
    {
        return found->second;
    }

    ComPtr<IShaderProgram> program =
        composeProgram(m_context, m_module, {vertexEntryPoint, fragmentEntryPoint});

    rhi::RenderPipelineDesc pipelineDesc = desc;
    pipelineDesc.program = program;
    ComPtr<IRenderPipeline> pipeline;
    checkRhi(
        m_context.device()->createRenderPipeline(pipelineDesc, pipeline.writeRef()),
        "create render pipeline"
    );

    return m_renderPipelines.emplace(key, pipeline).first->second;
}