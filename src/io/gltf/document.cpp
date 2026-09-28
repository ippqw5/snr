#include "./document.h"
#include "io/gltf/common.h"
#include <cstddef>

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>

namespace snr::gltf
{
namespace fs = std::filesystem;

static fs::path checkedPath(const std::string& filename, const fs::path& root)
{
    require(filename.find('\0') == std::string::npos, "NULL byte in asset path");
    auto path = fs::weakly_canonical(filename);
    auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    require(mismatch.first == root.end(), "Asset files must stay inside the model directory");
    return path;
}

static bool decodeUri(const std::string& uri, std::string* decoded, void*)
{
    if (!tinygltf::URIDecode(uri, decoded, nullptr))
        return false;
    require(decoded->find_first_of(":\\") == std::string::npos && decoded->find('\0') == std::string::npos,
            "Only relative local assets URIs are supported");

    fs::path path(*decoded);
    require(!path.is_absolute(), "Absolute asset URIs are not supported");
    for (const auto& part : path)
    {
        require(part != "..", "Assets URIs must stay inside the model directory");
    }
    return true;
}

static bool decodeImage(tinygltf::Image* image, int index, std::string* error, std::string* warning, int requestedWidth, int requestedHeight, const unsigned char* bytes, int size, void*)
{
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes, size, &width, &height, &channels) || width <= 0 || height <= 0 || size_t(width) * size_t(height) > maxTexturePixel)
    {
        *error += "Invalid texture or texture exceeds 16 million pixel limits";
        return false;
    }

    return tinygltf::LoadImageData(image, index, error, warning, requestedWidth, requestedHeight, bytes, size, nullptr);
}

tinygltf::Model readDocument(const std::filesystem::path& input, std::vector<std::string>& warnings)
{
    const auto path = fs::canonical(input);
    const auto root = path.parent_path();

    tinygltf::Model       model;
    tinygltf::TinyGLTF    loader;
    std::string           error, warning;
    tinygltf::FsCallbacks files;
    files.FileExists = [root](const std::string& name, void*) {
        return fs::is_regular_file(checkedPath(name, root));
    };
    files.ExpandFilePath = [](const std::string& name, void*) { return name; };
    files.ReadWholeFile = [root](std::vector<unsigned char>* bytes, std::string* error, const std::string& name, void*) {
        return tinygltf::ReadWholeFile(bytes, error, checkedPath(name, root).string(), nullptr);
    };
    files.GetFileSizeInBytes = [root](size_t* size, std::string* error, const std::string& name, void*) {
        return tinygltf::GetFileSizeInBytes(size, error, checkedPath(name, root).string(), nullptr);
    };
    files.WriteWholeFile = [](std::string* error, const std::string&, const std::vector<unsigned char>&, void*) {
        *error = "The glTF importer is read-only";
        return false;
    };
    files.user_data = nullptr;

    require(loader.SetFsCallbacks(std::move(files), &error), error);
    require(loader.SetURICallbacks({nullptr, decodeUri, nullptr}, &error), error);
    loader.SetImageLoader(decodeImage, nullptr);

    const bool binary = path.extension() == ".glb";
    require(binary || path.extension() == ".gltf", "Expected a .gltf or .glb file");
    bool loaded = binary ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
                         : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
    require(loaded, "Could not load glTF " + path.string() + ": " + error);
    require(model.asset.version == "2.0", "glTF 2.0 required");
    if (!warning.empty())
        warnings.push_back(warning);
    if (!model.animations.empty())
        warnings.push_back("Animations are not evaluated; rendering the static node transform");

    for (const auto& name : model.extensionsRequired)
    {
        require(name == "KHR_texture_transform" || name == "KHR_materials_emissive_strength", "Unsupported required glTF extension: " + name);
    }
    return model;
}
} // namespace snr::gltf