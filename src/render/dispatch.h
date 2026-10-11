#pragma once

#include "../gpu/device.h"
#include "shaders/scene_io.h.slang"

#include <slang-rhi.h>
#include <slang-rhi/shader-cursor.h>
#include <stdexcept>

namespace snr
{
// Writes a constants block into a pipeline's named constant buffer. The layout is checked before the
// copy, so a host-side change Slang does not see is reported instead of silently corrupted.
void bindConstants(const rhi::ShaderCursor& cursor, const char* name, const void* data, size_t size);

inline void bindParams(const rhi::ShaderCursor& cursor, const Params& params)
{
    bindConstants(cursor, "params", &params, sizeof(params));
}

// Runs one compute pipeline over the image extent named by Params::image and waits for it.
// `bindResources` receives the pipeline's root cursor to bind whatever the entry point reads.
template <typename BindResources>
void dispatchImage(
    DeviceContext&         context,
    rhi::IComputePipeline* pipeline,
    const Params&          params,
    BindResources          bindResources
)
{
    auto encoder = context.createEncoder();
    encoder->globalBarrier();
    auto* pass = encoder->beginComputePass();
    if (!pass)
        throw std::runtime_error("begin compute pass failed");
    auto* root = pass->bindPipeline(pipeline);
    if (!root)
        throw std::runtime_error("bind compute pipeline failed");
    rhi::ShaderCursor cursor(root);
    bindParams(cursor, params);
    bindResources(cursor);
    pass->dispatchCompute((params.image.x + 7) / 8, (params.image.y + 7) / 8, 1);
    pass->end();
    encoder->globalBarrier();
    context.submitAndWait(encoder);
}

} // namespace snr
