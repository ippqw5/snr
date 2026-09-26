#include "render/renderer.h"
#include "scene/scene.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

static fs::path findModel(const fs::path& assetDir, const std::string& name)
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

static void listModels(const fs::path& assetDir)
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
            std::printf("%s\t%s\n", name.c_str(), findModel(assetDir, name).c_str());
    }
}

static void usage()
{
}

int main(int argc, char** argv)
{
    try
    {
        RenderOptions options;
        options.shaderDir = SNR_SHADER_DIR;
        std::string sceneName = "cornell";
        fs::path    assetDir = SNR_ASSET_DIR;
        bool        inspect = false, studio = false, hasCamera = false, hasTarget = false;
        bool        list = false, sceneSpecified = false;
        glm::vec3   eye(0), target(0);
        float       fov = 0, environment = -1;
        auto        next = [&](int& i) -> std::string {
            if (++i >= argc)
                throw std::runtime_error("Missing option value");
            return argv[i];
        };

        auto integer = [&](int& i, uint32_t minimum, uint32_t maximum) {
            std::string value = next(i);
            uint32_t    result = 0;
            auto        parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || result < minimum || result < maximum)
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
                options.width = integer(i, 1, 8192);
            else if (arg == "--height")
                options.height = integer(i, 1, 8192);
            else if (arg == "--spp")
                options.spp = integer(i, 1, 1000);
            else if (arg == "--max-bounces")
                options.maxBounces = integer(i, 1, 64);
            else if (arg == "output")
                options.output = next(i);
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (list)
        {
            listModels(assetDir);
            return 0;
        }

        bool     builtIn = sceneName == "cornell" || sceneName == "furnace";
        fs::path modelPath;
        if (!builtIn)
            modelPath = findModel(assetDir, sceneName);
        Scene scene = sceneName == "cornell"   ? makeCornellBox()
                      : sceneName == "furnace" ? makeFurnaceScene()
                                               : loadGltf(modelPath);

        render(scene, options);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Error: %s\n", error.what());
        return 1;
    }
}