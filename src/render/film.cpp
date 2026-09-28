#include "./film.h"

#include "./result.h"

#include <slang-rhi.h>
#include <stdexcept>

namespace snr
{

using namespace rhi;

Film::Film(DeviceContext& context, ImageSize size)
    : m_context(context)
{
    resize(size);
}

void Film::resize(ImageSize size)
{
    if (size == m_size)
        return;

    size_t bytes = size_t(size.width) * size_t(size.height) * sizeof(glm::vec4);
    auto   accumulation = makeBuffer(
        m_context.device(),
        bytes,
        sizeof(glm::vec4),
        BufferUsage::UnorderedAccess | BufferUsage::CopySource,
        ResourceState::UnorderedAccess
    );
    auto display = makeBuffer(
        m_context.device(),
        bytes,
        sizeof(glm::vec4),
        BufferUsage::UnorderedAccess | BufferUsage::CopySource,
        ResourceState::UnorderedAccess
    );

    m_accumulation = accumulation;
    m_display = display;
    m_size = size;
    reset();
}

RenderResult Film::readback() const
{
    if (m_samples == 0)
    {
        throw std::runtime_error("cannot read an empty film");
    }

    size_t       pixelCount = size_t(m_size.width) * size_t(m_size.height);
    RenderResult result;
    result.image = m_size;
    result.samples = m_samples;
    result.sums.resize(pixelCount);
    result.display.resize(pixelCount);
    checkRhi(
        m_context.device()
            ->readBuffer(m_accumulation, 0, pixelCount * sizeof(glm::vec4), result.sums.data()),
        "read linear film"
    );
    checkRhi(
        m_context.device()
            ->readBuffer(m_display, 0, pixelCount * sizeof(glm::vec4), result.display.data()),
        "read display image"
    );
    return result;
}
} // namespace snr
