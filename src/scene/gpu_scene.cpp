#include "./gpu_scene.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <slang-rhi/acceleration-structure-utils.h>

namespace snr
{
using namespace rhi;

GpuScene::GpuScene(DeviceContext& context, const Scene& scene)
    : m_lightCount(uint32_t(scene.lights.size()))
{
    if (scene.triangles.empty() || scene.primitives.empty() || scene.instances.empty())
    {
        throw std::runtime_error("cannot upload an empty scene to gpu");
    }

    auto* device = context.device();

    // One shared vertex buffer holding every primitive, three vertices per triangle in primitive
    // order. Each bottom-level structure covers its own slice through a buffer offset.
    std::vector<glm::vec3> vertices;
    vertices.reserve(scene.triangles.size() * 3);
    for (const Triangle& triangle : scene.triangles)
    {
        vertices.push_back(glm::vec3(triangle.p0));
        vertices.push_back(glm::vec3(triangle.p1));
        vertices.push_back(glm::vec3(triangle.p2));
    }
    m_vertices = makeBuffer(
        device,
        vertices.size() * sizeof(glm::vec3),
        sizeof(glm::vec3),
        BufferUsage::AccelerationStructureBuildInput,
        ResourceState::AccelerationStructureBuildInput,
        vertices.data()
    );

    buildAccelerationStructures(context, scene);

    m_triangles = upload(device, scene.triangles);
    m_primitives = upload(device, scene.primitives);
    m_instances = upload(device, scene.instances);
    m_materials = upload(device, scene.materials);
    m_lights = upload(device, scene.lights);
    m_texels = upload(device, scene.texels);

    std::printf(
        "Scene: %zu triangles, %zu primitives, %zu instances, %zu materials, %zu emissive triangles",
        scene.triangles.size(),
        scene.primitives.size(),
        scene.instances.size(),
        scene.materials.size(),
        scene.lights.size()
    );
    std::fflush(stdout);
}

void GpuScene::buildAccelerationStructures(DeviceContext& context, const Scene& scene)
{
    auto* device = context.device();

    const size_t primitiveCount = scene.primitives.size();
    m_blas.resize(primitiveCount);

    // Each build keeps its own scratch buffer, so all of them can share one encoder and submit.
    std::vector<ComPtr<IBuffer>> scratchBuffers(primitiveCount);

    auto encoder = context.createEncoder();
    encoder->globalBarrier();

    for (size_t i = 0; i < primitiveCount; ++i)
    {
        const Primitive& primitive = scene.primitives[i];

        // AccelerationStructureBuildInput is not default constructible, so it stays a local that
        // is alive for the build call that consumes it.
        AccelerationStructureBuildInput input = {};
        input.type = AccelerationStructureBuildInputType::Triangles;
        input.triangles.vertexBuffers[0] = BufferOffsetPair(
            m_vertices.get(),
            Offset(size_t(primitive.firstTriangle) * 3 * sizeof(glm::vec3))
        );
        input.triangles.vertexBufferCount = 1;
        input.triangles.vertexFormat = Format::RGB32Float;
        input.triangles.vertexCount = primitive.triangleCount * 3;
        input.triangles.vertexStride = sizeof(glm::vec3);
        input.triangles.flags = AccelerationStructureGeometryFlags::None;

        AccelerationStructureBuildDesc desc = {};
        desc.inputs = &input;
        desc.inputCount = 1;
        desc.flags = AccelerationStructureBuildFlags::PreferFastTrace;

        AccelerationStructureSizes sizes = {};
        checkRhi(device->getAccelerationStructureSizes(desc, &sizes), "get acceleration structure sizes");

        AccelerationStructureDesc allocDesc = {};
        allocDesc.kind = AccelerationStructureKind::BottomLevel;
        allocDesc.size = sizes.accelerationStructureSize;
        checkRhi(
            device->createAccelerationStructure(allocDesc, m_blas[i].writeRef()),
            "create acceleration structure"
        );

        scratchBuffers[i] = makeBuffer(
            device,
            std::max<size_t>(size_t(sizes.scratchSize), 1),
            0,
            BufferUsage::UnorderedAccess,
            ResourceState::UnorderedAccess
        );
        encoder->buildAccelerationStructure(desc, m_blas[i], nullptr, scratchBuffers[i], 0, nullptr);
    }

    // The top-level structure places one instance of a primitive per scene instance.
    std::vector<AccelerationStructureInstanceDescGeneric> instanceDescs(scene.instances.size());
    for (size_t i = 0; i < scene.instances.size(); ++i)
    {
        const Instance&                           instance = scene.instances[i];
        AccelerationStructureInstanceDescGeneric& desc = instanceDescs[i];

        for (int row = 0; row < 3; ++row)
        {
            desc.transform[row][0] = instance.transformRows[row].x;
            desc.transform[row][1] = instance.transformRows[row].y;
            desc.transform[row][2] = instance.transformRows[row].z;
            desc.transform[row][3] = instance.transformRows[row].w;
        }
        desc.instanceID = uint32_t(i);
        desc.instanceMask = 0xff;
        desc.instanceContributionToHitGroupIndex = 0;
        desc.flags = AccelerationStructureInstanceFlags::TriangleFacingCullDisable;
        desc.accelerationStructure = m_blas[instance.primitive]->getHandle();
    }

    const auto instanceType = getAccelerationStructureInstanceDescType(device);
    const auto instanceSize = getAccelerationStructureInstanceDescSize(instanceType);
    std::vector<uint8_t> instanceBytes(size_t(instanceSize) * instanceDescs.size());
    convertAccelerationStructureInstanceDescs(
        instanceDescs.size(),
        instanceType,
        instanceBytes.data(),
        instanceSize,
        instanceDescs.data(),
        sizeof(AccelerationStructureInstanceDescGeneric)
    );

    m_instanceData = makeBuffer(
        device,
        instanceBytes.size(),
        0,
        BufferUsage::AccelerationStructureBuildInput,
        ResourceState::AccelerationStructureBuildInput,
        instanceBytes.data()
    );

    AccelerationStructureBuildInput topInput = {};
    topInput.type = AccelerationStructureBuildInputType::Instances;
    topInput.instances.instanceBuffer = m_instanceData;
    topInput.instances.instanceCount = uint32_t(instanceDescs.size());
    topInput.instances.instanceStride = uint32_t(instanceSize);

    AccelerationStructureBuildDesc topDesc = {};
    topDesc.inputs = &topInput;
    topDesc.inputCount = 1;
    topDesc.flags = AccelerationStructureBuildFlags::PreferFastTrace;

    AccelerationStructureSizes topSizes = {};
    checkRhi(device->getAccelerationStructureSizes(topDesc, &topSizes), "get acceleration structure sizes");

    AccelerationStructureDesc topAllocDesc = {};
    topAllocDesc.kind = AccelerationStructureKind::TopLevel;
    topAllocDesc.size = topSizes.accelerationStructureSize;
    checkRhi(device->createAccelerationStructure(topAllocDesc, m_tlas.writeRef()), "create acceleration structure");

    auto topScratch = makeBuffer(
        device,
        std::max<size_t>(size_t(topSizes.scratchSize), 1),
        0,
        BufferUsage::UnorderedAccess,
        ResourceState::UnorderedAccess
    );

    // The top-level build references the bottom-level structures, so order them explicitly.
    encoder->globalBarrier();
    encoder->buildAccelerationStructure(topDesc, m_tlas, nullptr, topScratch, 0, nullptr);
    context.submitAndWait(encoder);
}

void GpuScene::bind(const ShaderCursor& root) const
{
    checkRhi(root["sceneBVH"].setBinding(m_tlas), "bind scene BVH");
    checkRhi(root["triangles"].setBinding(m_triangles), "bind triangles");
    checkRhi(root["primitives"].setBinding(m_primitives), "bind primitives");
    checkRhi(root["instances"].setBinding(m_instances), "bind instances");
    checkRhi(root["materials"].setBinding(m_materials), "bind materials");
    checkRhi(root["lights"].setBinding(m_lights), "bind lights");
    checkRhi(root["texels"].setBinding(m_texels), "bind texels");
}
} // namespace snr