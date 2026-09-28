#pragma once

#include "../gpu/device.h"
#include "./result.h"
#include "./settings.h"

#include <slang-rhi.h>

namespace snr
{

class Film
{
public:
    Film(DeviceContext& context, ImageSize size);
    Film(const Film&) = delete;
    Film operator=(const Film&) = delete;

    void resize(ImageSize size);
    void reset()
    {
        m_samples = 0;
    }
    void addSamples(uint32_t count)
    {
        m_samples += count;
    }

    uint32_t samples() const
    {
        return m_samples;
    }
    ImageSize size() const
    {
        return m_size;
    }
    rhi::IBuffer* accumulation() const
    {
        return m_accumulation;
    }
    rhi::IBuffer* display() const
    {
        return m_display;
    }

    RenderResult readback() const;

private:
    DeviceContext&            m_context;
    uint32_t                  m_samples = 0;
    ImageSize                 m_size{0, 0};
    rhi::ComPtr<rhi::IBuffer> m_accumulation, m_display;
};

} // namespace snr
