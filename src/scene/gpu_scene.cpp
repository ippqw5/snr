#include "./gpu_scene.h"

#include <cstdio>
#include <cstring>
#include <slang-rhi/acceleration-structure-utils.h>
#include <stdexcept>

namespace snr
{
using namespace rhi;

static ComPtr<IAccelerationStructure> buildAS(
    DeviceContext&                         context,
    const AccelerationStructureBuildInput& input,
    AccelerationStructureKind              kind
)
{
    auto*                          device = context.device();
    AccelerationStructureBuildDesc buildDesc = {};
    buildDesc.inputs = &input;
    buildDesc.inputCount = 1;
    buildDesc.flags = AccelerationStructureBuildFlags::PreferFastTrace;
    AccelerationStructureSizes sizes = {};
    checkRhi(
        device->getAccelerationStructureSizes(buildDesc, &sizes),
        "get acceleration structure sizes"
    );

    AccelerationStructureDesc allocDesc = {};
    allocDesc.kind = kind;
    allocDesc.size = sizes.accelerationStructureSize;
    ComPtr<IAccelerationStructure> alloc;
    checkRhi(
        device->createAccelerationStructure(allocDesc, alloc.writeRef()),
        "create acceleration structure"
    );

    auto scratch = makeBuffer(
        device,
        sizes.scratchSize,
        0,
        BufferUsage::UnorderedAccess,
        ResourceState::UnorderedAccess
    );
    auto encoder = context.createEncoder();
    encoder->globalBarrier();
    encoder->buildAccelerationStructure(buildDesc, alloc, nullptr, scratch, 0, nullptr);
    context.submitAndWait(encoder);

    return alloc;
}

GpuScene::GpuScene(DeviceContext& context, const Scene& scene)
    : m_lightCount(uint32_t(scene.lights.size()))
{
    if (scene.triangles.empty())
    {
        throw std::runtime_error("cannot upload an empty scene to gpu");
    }

    auto*                  device = context.device();
    std::vector<glm::vec3> vertices;
    vertices.reserve(scene.triangles.size() * 3);
    for (const auto& triangle : scene.triangles)
    {
        vertices.push_back(glm::vec3(triangle.p0));
        vertices.push_back(glm::vec3(triangle.p1));
        vertices.push_back(glm::vec3(triangle.p2));
    }

    auto vertexBuffer = makeBuffer(
        device,
        vertices.size() * sizeof(glm::vec3),
        sizeof(glm::vec3),
        BufferUsage::AccelerationStructureBuildInput,
        ResourceState::AccelerationStructureBuildInput,
        vertices.data()
    );

    AccelerationStructureBuildInput input = {};
    input.type = AccelerationStructureBuildInputType::Triangles;
    input.triangles.vertexBuffers[0] = vertexBuffer;
    input.triangles.vertexBufferCount = 1;
    input.triangles.vertexFormat = Format::RGB32Float;
    input.triangles.vertexCount = uint32_t(vertices.size());
    input.triangles.vertexStride = sizeof(glm::vec3);
    input.triangles.flags = AccelerationStructureGeometryFlags::None;
    m_blas = buildAS(context, input, AccelerationStructureKind::BottomLevel);

    AccelerationStructureInstanceDescGeneric instance = {};
    instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1;
    instance.instanceMask = 0xff;
    instance.flags = AccelerationStructureInstanceFlags::TriangleFacingCullDisable;
    instance.accelerationStructure = m_blas->getHandle();

    auto                 instanceType = getAccelerationStructureInstanceDescType(device);
    auto                 instanceSize = getAccelerationStructureInstanceDescSize(instanceType);
    std::vector<uint8_t> instanceBytes(instanceSize);
    convertAccelerationStructureInstanceDescs(
        1,
        instanceType,
        instanceBytes.data(),
        instanceSize,
        &instance,
        sizeof(instance)
    );
    auto instanceBuffer = makeBuffer(
        device,
        instanceSize,
        0,
        BufferUsage::AccelerationStructureBuildInput,
        ResourceState::AccelerationStructureBuildInput,
        instanceBytes.data()
    );

    AccelerationStructureBuildInput topInput = {};
    topInput.type = AccelerationStructureBuildInputType::Instances;
    topInput.instances.instanceBuffer = instanceBuffer;
    topInput.instances.instanceCount = 1;
    topInput.instances.instanceStride = uint32_t(instanceSize);
    m_tlas = buildAS(context, topInput, AccelerationStructureKind::TopLevel);
    m_triangles = upload(device, scene.triangles);
    m_materials = upload(device, scene.materials);
    m_lights = upload(device, scene.lights);
    m_texels = upload(device, scene.texels);
    std::printf(
        "Scene: %zu triangles, %zu materials, %zu emissive triangles",
        scene.triangles.size(),
        scene.materials.size(),
        scene.lights.size()
    );
    std::fflush(stdout);
}

void GpuScene::bind(const ShaderCursor& root) const
{
    checkRhi(root["sceneBVH"].setBinding(m_tlas), "bind scene BVH");
    checkRhi(root["triangles"].setBinding(m_triangles), "bind triangles");
    checkRhi(root["materials"].setBinding(m_materials), "bind materials");
    checkRhi(root["lights"].setBinding(m_lights), "bind lights");
    checkRhi(root["texels"].setBinding(m_texels), "bind texels");
}
} // namespace snr
