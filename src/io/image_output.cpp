#include "./image_output.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace fs = std::filesystem;

void saveImages(const OutputSettings& options, const RenderResult& result)
{
    size_t count = size_t(result.imageSize.width) * size_t(result.imageSize.height);

    if (result.samples == 0 || result.sums.size() != count || result.display.size() != count)
    {
        throw std::runtime_error("Image Output requires a complete, nonempty render result");
    }

    if (options.path.empty() || options.path.extension() != ".png")
    {
        throw std::runtime_error("Image Output requires a .png path");
    }

    const auto&                sums = result.sums;
    const auto&                display = result.display;
    std::vector<float>         linear(count * 3);
    std::vector<unsigned char> rgba(count * 4);
    double                     sum = 0;
    float                      maximum = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (sums[i].w != float(result.samples))
        {
            throw std::runtime_error(
                "GPU sample count dismatch at pixel " + std::to_string(i) + ": expected " +
                std::to_string(result.samples) + ", got " + std::to_string(sums[i].w)
            );
        }

        for (int c = 0; c < 3; ++c)
        {
            float value = sums[i][c] / sums[i].w;
            if (!std::isfinite(value) || value < 0 || !std::isfinite(display[i][c]))
            {
                throw std::runtime_error("Non-finite or negative pixel radiance");
            }

            linear[i * 3 + c] = value;
            sum += value;
            maximum = std::max(maximum, value);
            rgba[i * 4 + c] =
                static_cast<unsigned char>(std::clamp(display[i][c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        rgba[i * 4 + 3] = 255;
    }

    auto output = fs::absolute(options.path);
    fs::create_directories(output.parent_path());

    auto png = output;
    if (!stbi_write_png(
            png.c_str(),
            result.imageSize.width,
            result.imageSize.height,
            4,
            rgba.data(),
            result.imageSize.width * 4
        ))
    {
        throw std::runtime_error("Failed to write PNG: " + png.string());
    }

    auto hdr = output;
    hdr.replace_extension(".hdr");
    if (!stbi_write_hdr(
            hdr.c_str(),
            result.imageSize.width,
            result.imageSize.height,
            3,
            linear.data()
        ))
    {
        throw std::runtime_error("Failed to write HDR: " + hdr.string());
    }

    std::printf(
        "Linear RGB mean: %.6f; maximum: %.6f\n PNG: %s\nHDR: %s\n",
        sum / (count * 3),
        maximum,
        png.c_str(),
        hdr.c_str()
    );
}