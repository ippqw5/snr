#include "./gpu_scene.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <slang-rhi/acceleration-structure-utils.h>

namespace snr
{
using namespace rhi;

namespace
{
TextureAddressingMode toAddressing(TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return TextureAddressingMode::ClampToEdge;
    case TextureWrap::MirrorRepeat:
        return TextureAddressingMode::MirrorRepeat;
    default:
        return TextureAddressingMode::Wrap;
    }
}
} // namespace

GpuScene::GpuScene(DeviceContext& context, const Scene& scene)
    : m_lightCount(uint32_t(scene.lights.size()))
{
    if (scene.indices.empty() || scene.primitives.empty() || scene.instances.empty())
    {
        throw std::runtime_error("cannot upload an empty scene to gpu");
    }

    auto* device = context.device();

    // The positions and indices are shared with the acceleration structures: the same data the
    // traversal reads is what the shader indexes, so no second copy is uploaded and the elements keep
    // their tight glTF sizes (float3 is 12 bytes) instead of being padded to float4.
    const BufferUsage geometryUsage =
        BufferUsage::ShaderResource | BufferUsage::AccelerationStructureBuildInput;
    m_positions = makeBuffer(
        device,
        scene.positions.size() * sizeof(glm::vec3),
        sizeof(glm::vec3),
        geometryUsage,
        ResourceState::ShaderResource,
        scene.positions.data()
    );
    m_indices = makeBuffer(
        device,
        scene.indices.size() * sizeof(glm::uvec3),
        sizeof(glm::uvec3),
        geometryUsage,
        ResourceState::ShaderResource,
        scene.indices.data()
    );
    m_normals = upload(device, scene.normals);
    m_baseColorUvs = upload(device, scene.baseColorUvs);
    m_metallicRoughnessUvs = upload(device, scene.metallicRoughnessUvs);
    m_normalUvs = upload(device, scene.normalUvs);
    m_occlusionUvs = upload(device, scene.occlusionUvs);
    m_emissiveUvs = upload(device, scene.emissiveUvs);

    buildAccelerationStructures(context, scene);

    m_primitives = upload(device, scene.primitives);
    m_instances = upload(device, scene.instances);
    m_materials = upload(device, scene.materials);
    m_lights = upload(device, scene.lights);
    m_triangleLights = upload(device, scene.triangleLights);
    m_lightCdf = upload(device, scene.lightCdf);

    // Each image becomes one hardware texture with its full mip chain, and each distinct sampler
    // state one sampler object. The material records name them by index, so the shader's bindless
    // arrays are filled in the same order the importer registered them.
    m_textures.reserve(scene.textures.size());
    m_textureViews.reserve(scene.textures.size());
    m_textureHandles.reserve(scene.textures.size());
    for (const TextureData& source : scene.textures)
    {
        TextureDesc desc = {};
        desc.type = TextureType::Texture2D;
        desc.size = {uint32_t(source.width), uint32_t(source.height), 1};
        desc.mipCount = uint32_t(source.levels.size());
        desc.format = source.srgb ? Format::RGBA8UnormSrgb : Format::RGBA8Unorm;
        desc.usage = TextureUsage::ShaderResource;

        // Subresources run mip-major; a plain 2D texture has one subresource per level.
        std::vector<SubresourceData> subresources(source.levels.size());
        for (size_t level = 0; level < source.levels.size(); ++level)
        {
            const uint32_t width = std::max(1u, uint32_t(source.width) >> level);
            subresources[level].data = source.levels[level].data();
            subresources[level].rowPitch = width * 4;
            subresources[level].slicePitch = 0;
        }

        ComPtr<ITexture> image;
        checkRhi(device->createTexture(desc, subresources.data(), image.writeRef()), "create texture");
        ComPtr<ITextureView> view;
        checkRhi(image->createView({}, view.writeRef()), "create texture view");
        DescriptorHandle handle = {};
        checkRhi(view->getDescriptorHandle(DescriptorHandleAccess::Read, &handle), "texture descriptor handle");

        m_textures.push_back(image);
        m_textureViews.push_back(view);
        m_textureHandles.push_back(handle);
    }

    m_samplers.reserve(scene.samplers.size());
    m_samplerHandles.reserve(scene.samplers.size());
    for (const TextureSampler& source : scene.samplers)
    {
        const TextureFilteringMode filter =
            source.nearest ? TextureFilteringMode::Point : TextureFilteringMode::Linear;

        SamplerDesc desc = {};
        desc.minFilter = filter;
        desc.magFilter = filter;
        desc.mipFilter = filter;
        desc.addressU = toAddressing(source.wrapU);
        desc.addressV = toAddressing(source.wrapV);
        desc.addressW = desc.addressU;

        ComPtr<ISampler> sampler;
        checkRhi(device->createSampler(desc, sampler.writeRef()), "create sampler");
        DescriptorHandle handle = {};
        checkRhi(sampler->getDescriptorHandle(&handle), "sampler descriptor handle");

        m_samplers.push_back(sampler);
        m_samplerHandles.push_back(handle);
    }

    std::printf(
        "Scene: %zu triangles, %zu vertices, %zu primitives, %zu instances, %zu materials, %zu textures, "
        "%zu samplers, %zu emissive triangles",
        scene.indices.size(),
        scene.positions.size(),
        scene.primitives.size(),
        scene.instances.size(),
        scene.materials.size(),
        scene.textures.size(),
        scene.samplers.size(),
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
        // Every primitive shares the one vertex buffer, so only this primitive's window of it is
        // described: the vertex data starts at vertexBase, and maxVertex (derived by the backend from
        // vertexCount) must stay inside the window or the index buffer would be read out of range.
        input.triangles.vertexBuffers[0] = BufferOffsetPair(
            m_positions.get(),
            Offset(size_t(primitive.vertexBase) * sizeof(glm::vec3))
        );
        input.triangles.vertexBufferCount = 1;
        input.triangles.vertexFormat = Format::RGB32Float;
        input.triangles.vertexCount = primitive.vertexCount;
        input.triangles.vertexStride = sizeof(glm::vec3);
        // Each index is a uint32, so triangle t starts at index (firstTriangle + t) * 3. The indices
        // are local to this primitive's window, which is why a distinct vertex base is needed.
        input.triangles.indexBuffer = BufferOffsetPair(
            m_indices.get(),
            Offset(size_t(primitive.firstTriangle) * 3 * sizeof(uint32_t))
        );
        input.triangles.indexFormat = IndexFormat::Uint32;
        input.triangles.indexCount = primitive.triangleCount * 3;
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
    checkRhi(root["positions"].setBinding(m_positions), "bind positions");
    checkRhi(root["normals"].setBinding(m_normals), "bind normals");
    checkRhi(root["baseColorUvs"].setBinding(m_baseColorUvs), "bind base color uvs");
    checkRhi(root["metallicRoughnessUvs"].setBinding(m_metallicRoughnessUvs), "bind metallic-roughness uvs");
    checkRhi(root["normalUvs"].setBinding(m_normalUvs), "bind normal uvs");
    checkRhi(root["occlusionUvs"].setBinding(m_occlusionUvs), "bind occlusion uvs");
    checkRhi(root["emissiveUvs"].setBinding(m_emissiveUvs), "bind emissive uvs");
    checkRhi(root["indices"].setBinding(m_indices), "bind indices");
    checkRhi(root["primitives"].setBinding(m_primitives), "bind primitives");
    checkRhi(root["instances"].setBinding(m_instances), "bind instances");
    checkRhi(root["materials"].setBinding(m_materials), "bind materials");
    checkRhi(root["lights"].setBinding(m_lights), "bind lights");
    checkRhi(root["triangleLights"].setBinding(m_triangleLights), "bind triangle lights");
    checkRhi(root["lightCdf"].setBinding(m_lightCdf), "bind light cdf");

    // The bindless heaps are plain arrays of handles, so each texture and sampler is written into the
    // slot its index names.
    auto images = root["images"];
    for (size_t index = 0; index < m_textureHandles.size(); ++index)
        checkRhi(images[uint32_t(index)].setDescriptorHandle(m_textureHandles[index]), "bind texture");

    auto samplers = root["samplers"];
    for (size_t index = 0; index < m_samplerHandles.size(); ++index)
        checkRhi(samplers[uint32_t(index)].setDescriptorHandle(m_samplerHandles[index]), "bind sampler");
}
} // namespace snr