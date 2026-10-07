#include "io/gltf/gltf_loader.h"
#include "render/renderer.h"
#include "scene/scene.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#define DEFAULT_OUTPUT_PATH "output/test.png"

namespace
{
namespace fs = std::filesystem;

fs::path findModel(const fs::path& assetDir, const std::string& name)
{
    if (fs::is_regular_file(name))
        return fs::canonical(name);

    fs::path modelName(name);
    if (modelName.has_parent_path() || modelName.has_extension())
        throw std::runtime_error("Model file not found: " + name);

    fs::path modelDir = assetDir / modelName;
    for (const auto& format : {std::string("glTF-Binary"), std::string("glTF")})
    {
        fs::path path = modelDir / format / (name + (format == "glTF" ? ".gltf" : ".glb"));
        if (fs::is_regular_file(path))
            return fs::canonical(path);
    }

    throw std::runtime_error("Model not available: " + name + ". Use --list-models, --asset-dir");
}

void listModels(const fs::path& assetDir)
{
    if (!fs::is_directory(assetDir))
        throw std::runtime_error("Asset directory not found: " + assetDir.string());

    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(assetDir))
    {
        if (entry.is_directory())
            names.push_back(entry.path().filename().string());
    }

    std::sort(names.begin(), names.end());

    for (const auto& name : names)
    {
        const auto binary = assetDir / name / "glTF-Binary" / (name + ".glb");
        const auto text = assetDir / name / "glTF" / (name + ".gltf");
        if (fs::is_regular_file(binary) || fs::is_regular_file(text))
            std::printf(
                "%s\t%s\n",
                name.c_str(),
                findModel(assetDir, name).string().c_str()
            );
    }
}

void usage()
{
    std::puts("snr: headless Vulkan/Slang path tracer\n"
              "Usage: pathtracer [options]\n"
              "  --output PATH.png        rendering output path\n"
              "  --scene cornell|furnace  Built-in scenes\n"
              "  --model NAME_OR_PATH     Khronos model name or ./gltf/.glb file\n"
              "  --asset-dir PATH         Models directory for name lookup\n"
              "  --list-models            List locally checked-out models\n"
              "  --width N --height N --spp N --max-bounces N\n"
              "  --seed N --batch-size N --exposure STOPS\n"
              "  --camera X Y Z --target X Y Z --fov DEGREES\n"
              "  --environment VALUE      Constant linear environment radiance\n"
              "  --studio                 Add floor and area light to a gltf model\n"
              "  --pfm                    Also write full-precision linear PATH.pfm\n"
              "  --shader-dir PATH        defaults to the source shader directory\n"
              "  --no-nee                 Disable area-light next-event estimation\n"
              "  --validate               Enable RHI and Vulkan validation\n"
              "  --inspect                Print scene Json without creating a GPU device\n"
              "  --help");
}

} // namespace

int main(int argc, char** argv)
{
    using namespace snr;
    try
    {
        RenderOptions options;
        options.shaderDir = SNR_SHADER_DIR;
        options.output.path = DEFAULT_OUTPUT_PATH;
        std::string              sceneName = "cornell";
        fs::path                 assetDir = SNR_ASSET_DIR;
        bool                     inspect = false, studio = false;
        bool                     list = false, sceneSpecified = false;
        std::optional<glm::vec3> eye, target;
        std::optional<float>     fov, environment;
        auto                     next = [&](int& i) -> std::string {
            if (++i >= argc)
                throw std::runtime_error("Missing option value");
            return argv[i];
        };

        auto integer = [&](int& i, uint32_t minimum, uint32_t maximum) {
            std::string value = next(i);
            uint32_t    result = 0;
            auto        parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || result < minimum || result > maximum)
                throw std::runtime_error("Integer option out of range: " + value);
            return result;
        };

        auto real = [&](int& i, float minimum, float maximum) {
            std::string value = next(i);
            char*       end = nullptr;
            float       result = std::strtof(value.c_str(), &end);
            if (end != value.c_str() + value.size() || end == value.c_str() || !std::isfinite(result) || result < minimum || result > maximum)
                throw std::runtime_error("Floating-point option out of range: " + value);
            return result;
        };

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--help")
            {
                usage();
                return 0;
            }
            else if (arg == "--scene" || arg == "--model")
            {
                if (sceneSpecified)
                    throw std::runtime_error("Choose only one --scene or --model");

                sceneName = next(i);
                sceneSpecified = true;
            }
            else if (arg == "--asset-dir")
                assetDir = next(i);
            else if (arg == "--list-models")
                list = true;
            else if (arg == "--width")
                options.image.width = integer(i, 1, 8192);
            else if (arg == "--height")
                options.image.height = integer(i, 1, 8192);
            else if (arg == "--spp")
                options.spp = integer(i, 1, 1000);
            else if (arg == "--max-bounces")
                options.integrator.maxBounces = integer(i, 1, 64);
            else if (arg == "--seed")
                options.integrator.seed = integer(i, 1, UINT32_MAX);
            else if (arg == "--batch-size")
                options.batchSize = integer(i, 1, 256);
            else if (arg == "--exposure")
                options.display.exposure = real(i, -20, 20);
            else if (arg == "--fov")
                fov = real(i, 0, 150);
            else if (arg == "--environment")
                environment = real(i, 0, 10000);
            else if (arg == "--output")
                options.output.path = next(i);
            else if (arg == "--shader-dir")
                options.shaderDir = next(i);
            else if (arg == "--studio")
                studio = true;
            else if (arg == "--inspect")
                inspect = true;
            else if (arg == "--validate")
                options.validation = true;
            else if (arg == "--pfm")
                options.output.writePfm = true;
            else if (arg == "--no-nee")
                options.integrator.nee = false;
            else if (arg == "--camera" || arg == "--target")
            {
                float x = real(i, -1e10f, 1e10f);
                float y = real(i, -1e10f, 1e10f);
                float z = real(i, -1e10f, 1e10f);
                (arg == "--camera" ? eye : target) = vec3(x, y, z);
            }
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (list)
        {
            listModels(assetDir);
            return 0;
        }

        if (!options.output.path.empty() && options.output.path.extension() != ".png")
            throw std::runtime_error("--output requires a .png extension");
        if (studio && (sceneName == "cornell" || sceneName == "furnace"))
            throw std::runtime_error("--studio requires a glTF scene");

        bool     builtIn = sceneName == "cornell" || sceneName == "furnace";
        fs::path modelPath;
        if (!builtIn)
            modelPath = findModel(assetDir, sceneName);
        Scene scene = sceneName == "cornell"   ? makeCornellBox()
                      : sceneName == "furnace" ? makeFurnaceScene()
                                               : loadGltf(modelPath);
        if (studio)
            addStudio(scene);

        scene.eye = eye.value_or(scene.eye);
        scene.target = target.value_or(scene.target);
        scene.fov = fov.value_or(scene.fov);
        if (environment)
            scene.environment = vec3(*environment);

        render(scene, options);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Error: %s\n", error.what());
        return 1;
    }
}