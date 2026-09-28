#include "./accessors.h"
#include "io/gltf/common.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <stdexcept>

namespace snr::gltf
{
static_assert(std::endian::native == std::endian::little);

struct Elements
{
    std::span<const unsigned char> bytes;
    size_t                         stride;

    const unsigned char* operator[](size_t index) const
    {
        return bytes.data() + index * stride;
    }
};

static Elements elements(const tinygltf::Model& model, int viewIndex, size_t offset, size_t count, size_t elementSize, size_t componentSize, bool packed)
{
    const auto& view = at(model.bufferViews, viewIndex, "buffer view");
    const auto& buffer = at(model.buffers, view.buffer, "buffer").data;

    size_t stride = view.byteStride ? view.byteStride : elementSize;

    return {std::span(buffer).subspan(view.byteOffset + offset, view.byteLength - offset), stride};
}

template <typename T>
static T component(const unsigned char* bytes)
{
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t unsignedComponent(const unsigned char* bytes, int type)
{
    switch (type)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return *bytes;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return component<uint16_t>(bytes);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return component<uint32_t>(bytes);
    default:
        throw std::runtime_error("Indices must use unsigned integer components");
    }
}

static float floatComponent(const unsigned char* bytes, int type, bool normalized)
{
    switch (type)
    {
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return component<float>(bytes);
    case TINYGLTF_COMPONENT_TYPE_BYTE:
        return normalized ? std::max(-1.0f, component<int8_t>(bytes) / 127.0f) : float(component<int8_t>(bytes));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return normalized ? *bytes / 255.0f : float(*bytes);
    case TINYGLTF_COMPONENT_TYPE_SHORT:
        return normalized ? std::max(-1.0f, component<int16_t>(bytes) / 32767.0f) : float(component<int16_t>(bytes));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return normalized ? component<uint16_t>(bytes) / 65535.0f : float(component<uint16_t>(bytes));
    default:
        throw std::runtime_error("Unsupported vertex component type");
    }
}

std::vector<float> readAttribute(const tinygltf::Model& model, int index, int type)
{
    const auto& accessor = at(model.accessors, index, "accessor");
    require(accessor.type == type, "Incorrectly typed vertex attribute");

    size_t components = size_t(tinygltf::GetNumComponentsInType(uint32_t(type)));
    int    componentSize = tinygltf::GetComponentSizeInBytes(uint32_t(accessor.componentType));

    Elements base = {};
    if (accessor.bufferView >= 0)
    {
        base = elements(model, accessor.bufferView, accessor.byteOffset, accessor.count, components * componentSize, componentSize, false);
    }
    else
    {
        require(accessor.byteOffset == 0, "Accessor without a buffer view has an offset");
    }

    std::vector<float> values(accessor.count * components, 0);
    auto               copyElement = [&](const unsigned char* bytes, size_t destination) {
        for (size_t c = 0; c < components; ++c)
        {
            float value = floatComponent(bytes + c * componentSize, accessor.componentType, accessor.normalized);
            require(std::isfinite(value), "Non-finite vertex attribute");
            values[destination * components + c] = value;
        }
    };

    if (accessor.bufferView >= 0)
    {
        for (size_t i = 0; i < accessor.count; i++)
        {
            copyElement(base[i], i);
        }
    }

    if (accessor.sparse.isSparse)
    {
        const auto& sparse = accessor.sparse;
        require(sparse.count > 0 && size_t(sparse.count) <= accessor.count, "Invalid sparse count");
        int indexSize = tinygltf::GetComponentSizeInBytes(uint32_t(sparse.indices.componentType));
        require(indexSize > 0, "Invalid sparse index type");

        auto indices = elements(
            model,
            sparse.indices.bufferView,
            sparse.indices.byteOffset,
            size_t(sparse.count),
            size_t(indexSize),
            size_t(indexSize),
            true
        );

        auto replacements = elements(
            model,
            sparse.values.bufferView,
            sparse.values.byteOffset,
            size_t(sparse.count),
            components * componentSize,
            size_t(componentSize),
            true
        );

        uint32_t previous = 0;
        for (int i = 0; i < sparse.count; ++i)
        {
            uint32_t destination = unsignedComponent(indices[size_t(i)], sparse.indices.componentType);
            copyElement(replacements[size_t(i)], destination);
            previous = destination;
        }
    }
    return values;
}

std::vector<uint32_t> readIndices(const tinygltf::Model& model, int index, size_t vertexCount)
{
    size_t count = index < 0 ? vertexCount : at(model.accessors, index, "index accessor").count;

    std::vector<uint32_t> indices;
    if (index < 0)
    {
        indices.resize(count);
        std::iota(indices.begin(), indices.end(), 0u);
    }
    else
    {
        const auto& accessor = at(model.accessors, index, "index accessor");
        require(!accessor.sparse.isSparse, "Sparse index accessors are not supported yet");
        require(accessor.type == TINYGLTF_TYPE_SCALAR && !accessor.normalized, "Indices must be unnormalized scalar");

        int  size = tinygltf::GetComponentSizeInBytes(uint32_t(accessor.componentType));
        auto data = elements(
            model,
            accessor.bufferView,
            accessor.byteOffset,
            count,
            size,
            size,
            true
        );
        indices.resize(count);
        for (size_t i = 0; i < count; i++)
        {
            indices[i] = unsignedComponent(data[i], accessor.componentType);
        }
    }

    for (uint32_t value : indices)
    {
        require(value < vertexCount, "Triangle index is out of range");
    }
    return indices;
}
} // namespace snr::gltf