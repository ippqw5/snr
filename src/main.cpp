#include "render/renderer.h"
#include "scene/scene.h"
#include "scene/scene_json.h"
#include "util/command_line.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define DEFAULT_OUTPUT_PATH "output/test.png"

namespace
{
namespace fs = std::filesystem;

// Resolves a Khronos sample-asset name to a file, or accepts a direct path to one.
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

} // namespace

int main(int argc, char** argv)
{
    using namespace snr;
    try
    {
        RenderOptions options;
        options.shaderDir = SNR_SHADER_DIR;
        options.output.path = DEFAULT_OUTPUT_PATH;
        std::string              modelName;
        fs::path                 assetDir = SNR_ASSET_DIR;
        bool                     inspect = false, studio = false;
        bool                     list = false;
        std::optional<glm::vec3> eye, target;
        std::optional<float>     fov, environment;

        CommandLine cli(argc, argv, "snr: Vulkan/Slang path tracer and forward raster preview");
        cli.text("--model", "NAME_OR_PATH", "Khronos model name or ./gltf/.glb file", modelName);
        cli.text("--asset-dir", "PATH", "Models directory for name lookup", assetDir);
        cli.flag("--list-models", "List locally checked-out models", list);
        cli.integer("--width", "N", "Image width", options.image.width, 1, 8192);
        cli.integer("--height", "N", "Image height", options.image.height, 1, 8192);
        cli.integer("--spp", "N", "Samples per pixel", options.spp, 1, 1024 * 1024);
        cli.integer("--max-bounces", "N", "Maximum path length", options.integrator.maxBounces, 1, 64);
        cli.integer("--seed", "N", "Random seed", options.integrator.seed, 1, UINT32_MAX);
        cli.integer("--batch-size", "N", "Samples accumulated per dispatch", options.batchSize, 1, 256);
        cli.real("--exposure", "STOPS", "Display exposure in stops", options.display.exposure, -20, 20);
        cli.custom("--camera", "X Y Z", "Camera position", [&](CommandLine& c, int& index) {
            eye = vec3(
                c.nextReal(index, -1e10f, 1e10f),
                c.nextReal(index, -1e10f, 1e10f),
                c.nextReal(index, -1e10f, 1e10f)
            );
        });
        cli.custom("--target", "X Y Z", "Camera target", [&](CommandLine& c, int& index) {
            target = vec3(
                c.nextReal(index, -1e10f, 1e10f),
                c.nextReal(index, -1e10f, 1e10f),
                c.nextReal(index, -1e10f, 1e10f)
            );
        });
        cli.custom("--fov", "DEGREES", "Vertical field of view", [&](CommandLine& c, int& index) {
            fov = c.nextReal(index, 0, 150);
        });
        cli.custom("--environment", "VALUE", "Constant linear environment radiance", [&](CommandLine& c, int& index) {
            environment = c.nextReal(index, 0, 10000);
        });
        cli.text("--output", "PATH.png", "Rendering output path", options.output.path);
        cli.text("--shader-dir", "PATH", "Shader source directory", options.shaderDir);
        cli.flag("--studio", "Add floor and area light to a glTF model", studio);
        cli.flag("--raster", "Draw with the forward raster preview instead of the path tracer", [&] {
            options.mode = RenderingMode::Raster;
        });
        cli.flag("--window", "Show the raster preview in an interactive window (implies --raster)", [&] {
            options.windowed = true;
            options.mode = RenderingMode::Raster;
        });
        cli.flag("--no-nee", "Disable area-light next-event estimation", [&] { options.integrator.nee = false; });
        cli.flag("--validate", "Enable RHI and Vulkan validation", options.validation);
        cli.flag("--inspect", "Print scene Json without creating a GPU device", inspect);

        cli.parse();

        if (cli.helpRequested())
        {
            std::fputs(cli.usage().c_str(), stdout);
            return 0;
        }
        if (list)
        {
            listModels(assetDir);
            return 0;
        }

        if (!options.output.path.empty() && options.output.path.extension() != ".png")
            throw std::runtime_error("--output requires a .png extension");
        if (modelName.empty())
            throw std::runtime_error("No model specified; use --model NAME_OR_PATH or --list-models");

        fs::path modelPath = findModel(assetDir, modelName);

        Scene scene;
        bool  loaded = scene.loadGltf(modelPath);

        // Warnings explain recoverable problems, and often why a load failed, so print them first.
        for (const auto& warning : scene.warnings)
            std::fprintf(stderr, "Warning: %s\n", warning.c_str());

        if (!loaded)
        {
            for (const auto& error : scene.errors)
                std::fprintf(stderr, "Error: %s\n", error.c_str());
            return 1;
        }

        if (studio)
            addStudio(scene);

        scene.eye = eye.value_or(scene.eye);
        scene.target = target.value_or(scene.target);
        scene.fov = fov.value_or(scene.fov);
        if (environment)
            scene.environment = vec3(*environment);

        if (inspect)
        {
            std::fputs(sceneJson(scene).c_str(), stdout);
            return 0;
        }

        render(scene, options);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Error: %s\n", error.what());
        return 1;
    }
}
