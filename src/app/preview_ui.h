#pragma once

#include "../gpu/shader_library.h"
#include "../render/settings.h"
#include "./window.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ImDrawData;
struct ImGuiViewport;
struct ImVec2;

namespace snr
{
// Dear ImGui's GLFW platform backend plus a small slang-rhi draw-data renderer.
class PreviewUi
{
public:
    PreviewUi(DeviceContext& context, Window& window, const std::filesystem::path& directory);
    ~PreviewUi();
    PreviewUi(const PreviewUi&) = delete;
    PreviewUi&                           operator=(const PreviewUi&) = delete;
    void                                 update(uint32_t& view, const CameraSettings& camera, PreviewControls& controls, uint32_t samples);
    void                                 draw(rhi::ITexture* target, bool clear);
    void                                 drawPlatformWindows();
    bool                                 capturesMouse() const;
    bool                                 capturesKeyboard() const;
    std::optional<std::filesystem::path> takeModel();
    void                                 setModel(const std::filesystem::path& path, const std::string& sceneName = {});
    void                                 setLoading(bool loading, float fraction = -1, const char* stage = "Reading glTF and decoding textures");
    void                                 setError(const std::string& error)
    {
        m_error = error;
    }

private:
    struct ViewportData;
    static void createViewport(ImGuiViewport* viewport);
    static void destroyViewport(ImGuiViewport* viewport);
    static void resizeViewport(ImGuiViewport* viewport, ImVec2 size);
    static void renderViewport(ImGuiViewport* viewport, void*);
    static void presentViewport(ImGuiViewport* viewport, void*);
    void        configureViewport(ImGuiViewport* viewport);
    void        renderDrawData(const ImDrawData* data, rhi::ITexture* target, bool clear);
    struct Vertex
    {
        glm::vec4 position, color, uv;
    };
    struct Entry
    {
        std::filesystem::path path;
        bool                  directory;
    };
    void browse(const std::filesystem::path& directory);
    void fileBrowser(PreviewControls& controls);
    void axes(const CameraSettings& camera);

    Window&                              m_window;
    std::string                          m_cpu, m_modelPath;
    int                                  m_resolution[2] = {800, 800};
    bool                                 m_editingResolution = false;
    int                                  m_windowResolution[2] = {800, 800};
    bool                                 m_editingWindowResolution = false;
    DeviceContext&                       m_context;
    ShaderLibrary                        m_shaders;
    rhi::ComPtr<rhi::IRenderPipeline>    m_pipeline;
    rhi::ComPtr<rhi::ITexture>           m_font;
    rhi::ComPtr<rhi::ISampler>           m_sampler;
    rhi::Format                          m_format = rhi::Format::Undefined;
    std::vector<Entry>                   m_entries;
    std::filesystem::path                m_directory;
    std::optional<std::filesystem::path> m_pending;
    std::string                          m_model = "No model imported", m_error;
    std::string                          m_browserError;
    char                                 m_pathInput[4096]{};
    char                                 m_filter[128]{};
    bool                                 m_browser = false, m_axes = true, m_controlsOpen = true;
    bool                                 m_loading = false;
    float                                m_loadFraction = -1;
    double                               m_loadStarted = 0;
    uint64_t                             m_presentedFrames = 0;
    std::string                          m_loadStage;
};
} // namespace snr
