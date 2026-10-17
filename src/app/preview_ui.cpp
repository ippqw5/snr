#include "./preview_ui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <memory>
#include <thread>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <slang-rhi/glfw.h>
#include <slang-rhi/shader-cursor.h>
#include <stdexcept>

namespace snr
{
namespace fs = std::filesystem;
namespace
{
std::string utf8(const fs::path& path)
{
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string cpuName()
{
    std::array<int, 12> brand{};
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int registers[4]{};
    __cpuid(registers, int(0x80000000u));
    if (uint32_t(registers[0]) >= 0x80000004u)
        for (uint32_t i = 0; i < 3; ++i)
            __cpuid(brand.data() + i * 4, int(0x80000002u + i));
#elif defined(__i386__) || defined(__x86_64__)
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u)
        for (uint32_t i = 0; i < 3; ++i)
        {
            unsigned int a, b, c, d;
            __cpuid(0x80000002u + i, a, b, c, d);
            brand[i * 4] = int(a);
            brand[i * 4 + 1] = int(b);
            brand[i * 4 + 2] = int(c);
            brand[i * 4 + 3] = int(d);
        }
#endif
    std::string name(reinterpret_cast<const char*>(brand.data()), sizeof(brand));
    const auto  end = name.find('\0');
    if (end != std::string::npos)
        name.resize(end);
    const auto first = name.find_first_not_of(' ');
    if (first != std::string::npos)
    {
        name.erase(0, first);
        name.erase(name.find_last_not_of(' ') + 1);
        return name;
    }
    return "Unknown CPU (" + std::to_string(std::thread::hardware_concurrency()) + " logical processors)";
}
bool modelFile(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".gltf" || ext == ".glb";
}
} // namespace

PreviewUi::PreviewUi(DeviceContext& context, Window& window, const fs::path& directory)
    : m_window(window), m_cpu(cpuName()), m_context(context), m_shaders(context, "preview_ui.slang")
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    try
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
        // Detached panels remain visible and interactive when the main window is minimized.
        io.ConfigViewportsNoDefaultParent = true;
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 0;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        style.FrameRounding = 4;
        style.FramePadding = {8, 5};
        // Use the OS CJK font when available; filenames and paths remain UTF-8 throughout the UI.
        const char* cjkFont = "C:/Windows/Fonts/msyh.ttc";
        if (fs::is_regular_file(cjkFont))
            io.Fonts->AddFontFromFileTTF(cjkFont, 18, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        rhi::TextureDesc desc{};
        desc.type = rhi::TextureType::Texture2D;
        desc.size = {uint32_t(width), uint32_t(height), 1};
        desc.format = rhi::Format::RGBA8Unorm;
        desc.usage = rhi::TextureUsage::ShaderResource;
        desc.defaultState = rhi::ResourceState::ShaderResource;
        rhi::SubresourceData data{};
        data.data = pixels;
        data.rowPitch = width * 4;
        checkRhi(context.device()->createTexture(desc, &data, m_font.writeRef()), "create UI font");
        io.Fonts->SetTexID(1);
        rhi::SamplerDesc sampler{};
        sampler.minFilter = sampler.magFilter = rhi::TextureFilteringMode::Linear;
        checkRhi(context.device()->createSampler(sampler, m_sampler.writeRef()), "create UI sampler");
        if (!ImGui_ImplGlfw_InitForVulkan(window.nativeHandle(), true))
            throw std::runtime_error("initialize ImGui GLFW backend failed");
        io.BackendRendererName = "snr_slang_rhi";
        io.BackendRendererUserData = this;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports | ImGuiBackendFlags_RendererHasVtxOffset;
        auto& platform = ImGui::GetPlatformIO();
        platform.Renderer_CreateWindow = &PreviewUi::createViewport;
        platform.Renderer_DestroyWindow = &PreviewUi::destroyViewport;
        platform.Renderer_SetWindowSize = &PreviewUi::resizeViewport;
        platform.Renderer_RenderWindow = &PreviewUi::renderViewport;
        platform.Renderer_SwapBuffers = &PreviewUi::presentViewport;
        browse(directory.empty() ? fs::current_path() : directory);
    }
    catch (...)
    {
        ImGui::DestroyContext();
        throw;
    }
}

PreviewUi::~PreviewUi()
{
    m_context.queue()->waitOnHost();
    // Renderer resources must be released before GLFW destroys the corresponding native windows.
    ImGui::DestroyPlatformWindows();
    ImGui::GetIO().BackendRendererUserData = nullptr;
    ImGui::GetIO().BackendRendererName = nullptr;
    ImGui::GetIO().BackendFlags &= ~(ImGuiBackendFlags_RendererHasViewports | ImGuiBackendFlags_RendererHasVtxOffset);
    auto& platform = ImGui::GetPlatformIO();
    platform.Renderer_CreateWindow = nullptr;
    platform.Renderer_DestroyWindow = nullptr;
    platform.Renderer_SetWindowSize = nullptr;
    platform.Renderer_RenderWindow = nullptr;
    platform.Renderer_SwapBuffers = nullptr;
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

struct PreviewUi::ViewportData
{
    PreviewUi*                 owner = nullptr;
    rhi::ComPtr<rhi::ISurface> surface;
    rhi::ComPtr<rhi::ITexture> image;
    ImageSize                  size{0, 0};
};

void PreviewUi::createViewport(ImGuiViewport* viewport)
{
    auto* owner = static_cast<PreviewUi*>(ImGui::GetIO().BackendRendererUserData);
    auto  data = std::make_unique<ViewportData>();
    data->owner = owner;
    data->surface = owner->m_context.device()->createSurface(
        rhi::getWindowHandleFromGLFW(static_cast<GLFWwindow*>(viewport->PlatformHandle))
    );
    if (!data->surface)
        throw std::runtime_error("create detached panel surface failed");
    viewport->RendererUserData = data.release();
    owner->configureViewport(viewport);
}

void PreviewUi::destroyViewport(ImGuiViewport* viewport)
{
    auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
    if (!data)
        return;
    data->owner->m_context.queue()->waitOnHost();
    data->image.setNull();
    if (data->surface->getConfig())
        data->surface->unconfigure();
    delete data;
    viewport->RendererUserData = nullptr;
}

void PreviewUi::configureViewport(ImGuiViewport* viewport)
{
    auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
    if (!data)
        return;
    int width = 0, height = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(viewport->PlatformHandle), &width, &height);
    const ImageSize size{uint32_t(std::max(0, width)), uint32_t(std::max(0, height))};
    if (size == data->size)
        return;
    m_context.queue()->waitOnHost();
    data->image.setNull();
    if (size.width && size.height)
    {
        rhi::SurfaceConfig config{};
        config.width = size.width;
        config.height = size.height;
        config.vsync = false;
        checkRhi(data->surface->configure(config), "configure detached panel surface");
    }
    else if (data->surface->getConfig())
        checkRhi(data->surface->unconfigure(), "unconfigure detached panel surface");
    data->size = size;
}

void PreviewUi::resizeViewport(ImGuiViewport* viewport, ImVec2)
{
    if (auto* data = static_cast<ViewportData*>(viewport->RendererUserData))
        data->owner->configureViewport(viewport);
}

void PreviewUi::renderViewport(ImGuiViewport* viewport, void*)
{
    auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
    if (!data)
        return;
    data->owner->configureViewport(viewport);
    if (!data->surface->getConfig())
        return;
    data->image.setNull();
    if (SLANG_FAILED(data->surface->acquireNextImage(data->image.writeRef())))
        return;
    data->owner->renderDrawData(viewport->DrawData, data->image, true);
}

void PreviewUi::presentViewport(ImGuiViewport* viewport, void*)
{
    auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
    if (!data || !data->image)
        return;
    checkRhi(data->surface->present(), "present detached panel surface");
    data->image.setNull();
}

void PreviewUi::browse(const fs::path& directory)
{
    try
    {
        std::vector<Entry> entries;
        for (const auto& item : fs::directory_iterator(directory, fs::directory_options::skip_permission_denied))
        {
            std::error_code error;
            bool            dir = item.is_directory(error);
            if (!error && (dir || (item.is_regular_file(error) && modelFile(item.path()))))
                entries.push_back({item.path(), dir});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.directory != b.directory)
                return a.directory;
            return a.path.filename().wstring() < b.path.filename().wstring();
        });
        m_entries = std::move(entries);
        m_directory = fs::absolute(directory);
        std::snprintf(m_pathInput, sizeof(m_pathInput), "%s", utf8(m_directory).c_str());
        m_browserError.clear();
    }
    catch (const std::exception& error)
    {
        m_browserError = error.what();
    }
}

void PreviewUi::fileBrowser(PreviewControls& controls)
{
    ImGui::SetNextWindowSize({660, 480}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Import glTF / GLB", &m_browser))
    {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("Enter a folder or file path, or select a file below.");
    ImGui::Checkbox("Add studio floor and point light", &controls.studio);
    ImGui::TextDisabled("Applied when importing the selected model.");
    ImGui::SetNextItemWidth(-65);
    bool open = ImGui::InputText("##path", m_pathInput, sizeof(m_pathInput), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    open |= ImGui::Button("Open");
    if (open)
    {
        fs::path        path = std::u8string(reinterpret_cast<const char8_t*>(m_pathInput));
        std::error_code error;
        if (fs::is_directory(path, error))
            browse(path);
        else if (fs::is_regular_file(path, error) && modelFile(path))
        {
            m_pending = path;
            m_browser = false;
        }
        else
            m_browserError = "Select an existing folder, .gltf or .glb file.";
    }
    if (ImGui::Button("Parent folder"))
        browse(m_directory.parent_path());
    ImGui::SameLine();
    if (ImGui::Button("Working directory"))
        browse(fs::current_path());
#ifdef _WIN32
    for (char drive = 'A'; drive <= 'Z'; ++drive)
    {
        std::string     root = std::string(1, drive) + ":/";
        std::error_code error;
        if (fs::is_directory(root, error))
        {
            ImGui::SameLine();
            if (ImGui::Button(root.c_str()))
                browse(root);
        }
    }
#endif
    ImGui::InputTextWithHint("##filter", "Filter filenames", m_filter, sizeof(m_filter));
    if (!m_browserError.empty())
        ImGui::TextWrapped("%s", m_browserError.c_str());
    ImGui::Separator();
    std::optional<fs::path> directory;
    if (ImGui::BeginChild("Files", {0, 0}, ImGuiChildFlags_Borders))
    {
        if (m_entries.empty())
            ImGui::TextDisabled("No folders or glTF files in this directory.");
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            const auto& entry = m_entries[i];
            std::string name = utf8(entry.path.filename());
            if (m_filter[0] && name.find(m_filter) == std::string::npos)
                continue;
            ImGui::PushID(int(i));
            if (ImGui::Selectable(((entry.directory ? "[Folder] " : "[Import] ") + name).c_str()))
            {
                if (entry.directory)
                    directory = entry.path;
                else
                {
                    m_pending = entry.path;
                    m_browser = false;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
    if (directory)
        browse(*directory);
}

void PreviewUi::axes(const CameraSettings& camera)
{
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - 16,
                             viewport->WorkPos.y + viewport->WorkSize.y - 16},
                            ImGuiCond_Always, {1, 1});
    ImGui::SetNextWindowSize({150, 164}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("World axes", nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImVec2             pos = ImGui::GetWindowPos();
    ImVec2             center(pos.x + 75, pos.y + 76);
    constexpr float    radius = 47.0f;
    auto*              draw = ImGui::GetWindowDrawList();
    const glm::vec3    forward = glm::normalize(camera.target - camera.eye);
    const glm::vec3    right = glm::normalize(glm::cross(forward, camera.up));
    const glm::vec3    up = glm::cross(right, forward);
    const ImU32        colors[] = {IM_COL32(245, 75, 65, 255), IM_COL32(85, 225, 110, 255), IM_COL32(85, 145, 255, 255)};
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return forward[a] > forward[b]; });
    for (int axis : order)
    {
        ImVec2 end(center.x + right[axis] * radius, center.y - up[axis] * radius);
        draw->AddLine(center, end, colors[axis], 3);
        draw->AddCircleFilled(end, 4, colors[axis]);
        char label[] = {"XYZ"[axis], 0};
        draw->AddText({end.x + 6, end.y - 8}, colors[axis], label);
    }
    draw->AddCircleFilled(center, 4, IM_COL32(220, 225, 235, 255));
    draw->AddText({pos.x + 30, pos.y + 139}, IM_COL32(170, 180, 195, 255), "World axes");
    ImGui::End();
    ImGui::PopStyleVar();
}

void PreviewUi::update(uint32_t& view, const CameraSettings& camera, PreviewControls& controls, uint32_t samples)
{
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Panels"))
        {
            ImGui::MenuItem("Scene preview", nullptr, &m_controlsOpen);
            ImGui::BeginDisabled(m_loading);
            ImGui::MenuItem("Import glTF / GLB", nullptr, &m_browser);
            ImGui::EndDisabled();
            ImGui::MenuItem("World axes", nullptr, &m_axes);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    const auto* mainViewport = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(0, mainViewport, ImGuiDockNodeFlags_PassthruCentralNode);
    if (m_controlsOpen)
    {
        ImGui::SetNextWindowPos({mainViewport->WorkPos.x + 16, mainViewport->WorkPos.y + 16}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({320, 620}, ImGuiCond_FirstUseEver);
        const bool visible = ImGui::Begin("Scene preview", &m_controlsOpen, ImGuiWindowFlags_NoCollapse);
        if (visible)
        {
            ImGui::BeginDisabled(m_loading);
            if (ImGui::Button("Import glTF / GLB...", {-1, 0}))
                m_browser = true;
            ImGui::EndDisabled();
            ImGui::TextWrapped("Scene: %s", m_model.c_str());
            if (!m_modelPath.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", m_modelPath.c_str());
            ImGui::SeparatorText("Frame statistics");
            const float fps = ImGui::GetIO().Framerate;
            ImGui::Text("%.1f FPS | %.2f ms/frame", fps, fps > 0 ? 1000.0f / fps : 0.0f);
            ImGui::Text("Frames: %llu", static_cast<unsigned long long>(m_presentedFrames));
            ImGui::SeparatorText("Resolution");
            const auto framebuffer = m_window.framebufferSize();
            ImGui::Text("Window: %u x %u", framebuffer.width, framebuffer.height);
            const auto renderSize = controls.followWindow && framebuffer.width && framebuffer.height ? framebuffer : controls.resolution;
            ImGui::Text("Render: %u x %u", renderSize.width, renderSize.height);
            ImGui::BeginDisabled(m_loading);
            if (!m_editingWindowResolution && framebuffer.width && framebuffer.height)
            {
                m_windowResolution[0] = int(framebuffer.width);
                m_windowResolution[1] = int(framebuffer.height);
            }
            if (ImGui::InputInt2("Window width / height", m_windowResolution))
                m_editingWindowResolution = true;
            const bool validWindowSize = m_windowResolution[0] >= 1 && m_windowResolution[0] <= 4096 &&
                                         m_windowResolution[1] >= 1 && m_windowResolution[1] <= 4096;
            ImGui::BeginDisabled(!validWindowSize);
            if (ImGui::Button("Apply window size"))
            {
                m_window.requestFramebufferSize({uint32_t(m_windowResolution[0]), uint32_t(m_windowResolution[1])});
                m_editingWindowResolution = false;
            }
            ImGui::EndDisabled();
            if (!validWindowSize)
                ImGui::TextDisabled("Use 1-4096 pixels per dimension.");
            if (ImGui::Checkbox("Follow window size", &controls.followWindow))
            {
                controls.resolution = renderSize;
                m_editingResolution = false;
            }
            ImGui::BeginDisabled(controls.followWindow);
            if (!m_editingResolution)
            {
                m_resolution[0] = int(controls.resolution.width);
                m_resolution[1] = int(controls.resolution.height);
            }
            if (ImGui::InputInt2("Render width / height", m_resolution))
                m_editingResolution = true;
            const bool validSize = m_resolution[0] >= 1 && m_resolution[0] <= 4096 &&
                                   m_resolution[1] >= 1 && m_resolution[1] <= 4096;
            ImGui::BeginDisabled(!validSize);
            if (ImGui::Button("Apply resolution"))
            {
                controls.resolution = {uint32_t(m_resolution[0]), uint32_t(m_resolution[1])};
                m_editingResolution = false;
            }
            ImGui::EndDisabled();
            if (!validSize)
                ImGui::TextDisabled("Use 1-4096 pixels per dimension.");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SeparatorText("Renderer");
            ImGui::BeginDisabled(m_loading);
            int                   mode = controls.mode == RenderingMode::Raster ? 0 : 1;
            constexpr const char* renderers[] = {"Raster", "Path tracing"};
            if (ImGui::Combo("##renderer", &mode, renderers, 2))
                controls.mode = mode == 0 ? RenderingMode::Raster : RenderingMode::PathTracing;
            if (controls.mode == RenderingMode::PathTracing)
            {
                auto integer = [](const char* label, uint32_t& value, int maximum) {
                    int edited = int(value);
                    if (ImGui::InputInt(label, &edited))
                        value = uint32_t(std::clamp(edited, 1, maximum));
                };
                integer("Target SPP", controls.targetSamples, 1048576);
                integer("SPP / frame", controls.samplesPerFrame, 16);
                integer("Max bounces", controls.integrator.maxBounces, 64);
                ImGui::Checkbox("Direct light sampling (NEE)", &controls.integrator.nee);
                ImGui::Checkbox("Pause accumulation", &controls.paused);
                if (ImGui::Button("Restart accumulation"))
                    controls.restart = true;
                ImGui::Text("Samples: %u / %u", samples, controls.targetSamples);
                ImGui::ProgressBar(std::min(1.0f, float(samples) / controls.targetSamples), {-1, 0});
            }
            ImGui::EndDisabled();
            ImGui::SliderFloat("Exposure", &controls.display.exposure, -10, 10, "%.1f stops");
            ImGui::SeparatorText("View");
            ImGui::BeginDisabled(controls.mode != RenderingMode::Raster);
            constexpr const char* views[] = {"Shaded", "Depth", "Object ID", "Base Color", "Normal"};
            int                   selected = int(view);
            if (ImGui::Combo("##view", &selected, views, 5))
                view = uint32_t(selected);
            ImGui::EndDisabled();
            if (controls.mode == RenderingMode::PathTracing)
                ImGui::TextDisabled("Debug views: select Raster");
            ImGui::Checkbox("Show XYZ axes", &m_axes);
            ImGui::SeparatorText("Navigation");
            ImGui::TextUnformatted("Left drag: orbit\nRight drag: pan\nWheel: zoom\n1-5: switch view");
            if (!m_error.empty())
            {
                ImGui::SeparatorText("Operation failed");
                ImGui::TextWrapped("%s", m_error.c_str());
            }
            ImGui::SeparatorText("System Information");
            const auto& info = m_context.device()->getInfo();
            ImGui::TextWrapped("Graphics API: %s", info.apiName ? info.apiName : "Unknown");
            ImGui::TextWrapped("GPU: %s", info.adapterName ? info.adapterName : "Unknown");
            ImGui::TextWrapped("CPU: %s", m_cpu.c_str());
        }
        ImGui::End();
    }
    if (m_browser)
        fileBrowser(controls);
    if (m_axes)
        axes(camera);
    if (m_loading)
    {
        const auto center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({420, 0});
        ImGui::Begin("Loading glTF", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove);
        ImGui::TextWrapped("%s", m_loadStage.c_str());
        // Parsing/decoding has no measurable total. Animate that phase instead of inventing a percentage.
        ImGui::ProgressBar(m_loadFraction < 0 ? -float(ImGui::GetTime()) : m_loadFraction,
                           {-1, 0}, m_loadFraction < 0 ? "Working..." : nullptr);
        ImGui::Text("Elapsed: %.1f s", ImGui::GetTime() - m_loadStarted);
        ImGui::End();
    }
    ImGui::Render();
}

bool PreviewUi::capturesMouse() const
{
    return ImGui::GetIO().WantCaptureMouse;
}
bool PreviewUi::capturesKeyboard() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}
std::optional<fs::path> PreviewUi::takeModel()
{
    auto result = std::move(m_pending);
    m_pending.reset();
    return result;
}
void PreviewUi::setLoading(bool loading, float fraction, const char* stage)
{
    if (loading && !m_loading)
    {
        m_loadStarted = ImGui::GetTime();
        m_error.clear();
        m_browser = false;
    }
    m_loading = loading;
    m_loadFraction = fraction;
    m_loadStage = stage;
}

void PreviewUi::setModel(const fs::path& path, const std::string& sceneName)
{
    m_model = sceneName.empty() ? utf8(path.stem()) : sceneName;
    std::error_code error;
    const auto      absolute = fs::absolute(path, error);
    m_modelPath = utf8(error ? path : absolute);
    m_error.clear();
}

void PreviewUi::draw(rhi::ITexture* target, bool clear)
{
    renderDrawData(ImGui::GetDrawData(), target, clear);
    drawPlatformWindows();
}

void PreviewUi::drawPlatformWindows()
{
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
    ++m_presentedFrames;
}

void PreviewUi::renderDrawData(const ImDrawData* data, rhi::ITexture* target, bool clear)
{
    using namespace rhi;
    if (!data || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0)
        return;
    const auto format = target->getDesc().format;
    if (!m_pipeline || m_format != format)
    {
        ColorTargetDesc color{};
        color.format = format;
        color.enableBlend = true;
        color.color.srcFactor = BlendFactor::SrcAlpha;
        color.color.dstFactor = BlendFactor::InvSrcAlpha;
        color.alpha.srcFactor = BlendFactor::One;
        color.alpha.dstFactor = BlendFactor::InvSrcAlpha;
        RenderPipelineDesc desc{};
        desc.targets = &color;
        desc.targetCount = 1;
        desc.primitiveTopology = PrimitiveTopology::TriangleList;
        desc.rasterizer.cullMode = CullMode::None;
        m_pipeline = m_shaders.renderPipeline("uiVS", "uiFS", desc, std::to_string(int(format)));
        m_format = format;
    }
    // Expand indexed triangles once on the CPU; the vertex shader reads the structured buffer.
    std::vector<Vertex> vertices;
    vertices.reserve(data->TotalIdxCount);
    for (const ImDrawList* list : data->CmdLists)
        for (const auto& command : list->CmdBuffer)
        {
            if (command.UserCallback)
                continue;
            for (unsigned int i = 0; i < command.ElemCount; ++i)
            {
                const auto& v = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + i] + command.VtxOffset];
                glm::vec4   color(float((v.col >> IM_COL32_R_SHIFT) & 255) / 255,
                                  float((v.col >> IM_COL32_G_SHIFT) & 255) / 255,
                                  float((v.col >> IM_COL32_B_SHIFT) & 255) / 255,
                                  float((v.col >> IM_COL32_A_SHIFT) & 255) / 255);
                // The overlay shader outputs display colors. Decode for an sRGB swapchain.
                if (format == Format::RGBA8UnormSrgb || format == Format::BGRA8UnormSrgb || format == Format::BGRX8UnormSrgb)
                    for (int c = 0; c < 3; ++c)
                        color[c] = color[c] <= 0.04045f ? color[c] / 12.92f : std::pow((color[c] + 0.055f) / 1.055f, 2.4f);
                vertices.push_back({{2 * (v.pos.x - data->DisplayPos.x) / data->DisplaySize.x - 1,
                                     1 - 2 * (v.pos.y - data->DisplayPos.y) / data->DisplaySize.y, 0, 1},
                                    color,
                                    {v.uv.x, v.uv.y, 0, 0}});
            }
        }
    auto buffer = upload(m_context.device(), vertices);
    auto encoder = m_context.createEncoder();
    encoder->globalBarrier();
    RenderPassColorAttachment color{};
    color.view = target->getDefaultView();
    color.loadOp = clear ? LoadOp::Clear : LoadOp::Load;
    color.storeOp = StoreOp::Store;
    RenderPassDesc desc{};
    desc.colorAttachments = &color;
    desc.colorAttachmentCount = 1;
    auto* pass = encoder->beginRenderPass(desc);
    if (!pass)
        throw std::runtime_error("begin UI pass failed");
    auto* root = pass->bindPipeline(m_pipeline);
    if (!root)
        throw std::runtime_error("bind UI pipeline failed");
    ShaderCursor cursor(root);
    checkRhi(cursor["vertices"].setBinding(buffer), "bind UI vertices");
    checkRhi(cursor["fontTexture"].setBinding(m_font->getDefaultView()), "bind UI font");
    checkRhi(cursor["fontSampler"].setBinding(m_sampler), "bind UI sampler");
    auto size = target->getDesc().size;
    // Each native window can be on a monitor with a different framebuffer scale.
    const float scaleX = float(size.width) / data->DisplaySize.x;
    const float scaleY = float(size.height) / data->DisplaySize.y;
    RenderState state{};
    state.viewports[0] = Viewport::fromSize(size.width, size.height);
    state.viewportCount = 1;
    state.scissorRectCount = 1;
    uint32_t first = 0;
    for (const ImDrawList* list : data->CmdLists)
        for (const auto& command : list->CmdBuffer)
        {
            if (command.UserCallback)
                continue;
            int x0 = std::clamp(int((command.ClipRect.x - data->DisplayPos.x) * scaleX), 0, int(size.width));
            int y0 = std::clamp(int((command.ClipRect.y - data->DisplayPos.y) * scaleY), 0, int(size.height));
            int x1 = std::clamp(int((command.ClipRect.z - data->DisplayPos.x) * scaleX), 0, int(size.width));
            int y1 = std::clamp(int((command.ClipRect.w - data->DisplayPos.y) * scaleY), 0, int(size.height));
            if (x1 > x0 && y1 > y0)
            {
                state.scissorRects[0] = {uint32_t(x0), uint32_t(y0), uint32_t(x1), uint32_t(y1)};
                pass->setRenderState(state);
                DrawArguments args{};
                args.vertexCount = command.ElemCount;
                args.startVertexLocation = first;
                args.instanceCount = 1;
                pass->draw(args);
            }
            first += command.ElemCount;
        }
    pass->end();
    // The temporary upload must outlive all commands referencing it.
    m_context.submitAndWait(encoder);
    m_context.checkValidation();
}
} // namespace snr
