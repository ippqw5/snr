#pragma once

#include "../gpu/device.h"
#include "../render/settings.h"

#include <glm/glm.hpp>
#include <optional>
#include <string>

struct GLFWwindow;

namespace snr
{
// A GLFW window together with the slang-rhi surface that presents into it. The window owns the
// surface's configuration: it reconfigures on a framebuffer resize and reports the size, so the
// renderer keeps its targets in step.
//
class Window
{
public:
    Window(const std::string& title, ImageSize size, DeviceContext& context, bool vsync = true);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Processes pending events, updates the surface on a resize and refreshes the input state.
    // Returns false once the window should close.
    bool pump();

    // Framebuffer size in pixels; zero while the window is minimized.
    ImageSize framebufferSize() const
    {
        return m_size;
    }

    // Acquires the next swapchain image, or null when the surface is unconfigured or has none ready.
    rhi::ComPtr<rhi::ITexture> acquireImage();
    void                       present();
    void                       setTitle(const std::string& title);

    std::optional<uint32_t> numberKeyPressed() const
    {
        return m_numberKey;
    }

    glm::vec2 cursorDelta() const
    {
        return m_cursorDelta;
    }
    glm::vec2 scrollDelta() const
    {
        return m_scrollDelta;
    }
    bool leftButton() const
    {
        return m_leftButton;
    }
    bool rightButton() const
    {
        return m_rightButton;
    }

private:
    static void onScroll(GLFWwindow* window, double x, double y);
    static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods);

    void reconfigure(ImageSize size);

    DeviceContext&             m_context;
    GLFWwindow*                m_window = nullptr;
    rhi::ComPtr<rhi::ISurface> m_surface;
    ImageSize                  m_size{0, 0};
    bool                       m_vsync;

    glm::vec2               m_cursor{0, 0};
    glm::vec2               m_cursorDelta{0, 0};
    glm::vec2               m_scroll{0, 0};
    glm::vec2               m_scrollDelta{0, 0};
    bool                    m_hasCursor = false;
    bool                    m_leftButton = false;
    bool                    m_rightButton = false;
    std::optional<uint32_t> m_numberKey = 1;
};
} // namespace snr
