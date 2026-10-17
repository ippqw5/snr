#include "./window.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// GLFW only declares the native window accessors of the platform it is asked to expose.
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

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <slang-rhi/glfw.h>

namespace snr
{
using namespace rhi;

Window::Window(const std::string& title, ImageSize size, DeviceContext& context, bool vsync)
    : m_context(context), m_vsync(vsync)
{
    if (!glfwInit())
        throw std::runtime_error("failed to initialize GLFW");

    // Vulkan draws into the window, so GLFW must not create a graphics context of its own.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(int(size.width), int(size.height), title.c_str(), nullptr, nullptr);
    if (!m_window)
    {
        glfwTerminate();
        throw std::runtime_error("failed to create a GLFW window");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetScrollCallback(m_window, &Window::onScroll);
    glfwSetKeyCallback(m_window, &Window::onKey);

    m_surface = m_context.device()->createSurface(getWindowHandleFromGLFW(m_window));
    if (!m_surface)
    {
        glfwDestroyWindow(m_window);
        glfwTerminate();
        throw std::runtime_error("failed to create a swapchain surface");
    }

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    reconfigure({uint32_t(width), uint32_t(height)});
}

Window::~Window()
{
    if (m_surface)
        m_surface->unconfigure();
    m_surface.setNull();
    if (m_window)
    {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

void Window::onScroll(GLFWwindow* window, double, double y)
{
    static_cast<Window*>(glfwGetWindowUserPointer(window))->m_scroll += glm::vec2(0, float(y));
}

void Window::onKey(GLFWwindow* window, int key, int, int action, int)
{
    if (action != GLFW_PRESS)
        return;

    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        self->m_numberKey = uint32_t(key - GLFW_KEY_0);
    else if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
        self->m_numberKey = uint32_t(key - GLFW_KEY_KP_0);
}

void Window::reconfigure(ImageSize size)
{
    // The queue must be idle before the swapchain is torn down or resized.
    m_context.queue()->waitOnHost();

    if (size.width > 0 && size.height > 0)
    {
        SurfaceConfig config = {};
        config.width = size.width;
        config.height = size.height;
        config.vsync = m_vsync;
        checkRhi(m_surface->configure(config), "configure surface");
    }
    else
    {
        // A minimized window has no drawable area; skipping the frame is the documented behaviour.
        checkRhi(m_surface->unconfigure(), "unconfigure surface");
    }
    m_size = size;
}

bool Window::pump(bool captureKeyboard)
{
    m_numberKey.reset();

    glfwPollEvents();
    if (m_requestedSize)
    {
        const auto requested = *m_requestedSize;
        m_requestedSize.reset();
        if (glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) || glfwGetWindowAttrib(m_window, GLFW_ICONIFIED))
            glfwRestoreWindow(m_window);
        int logicalWidth = 0, logicalHeight = 0, pixelWidth = 0, pixelHeight = 0;
        glfwGetWindowSize(m_window, &logicalWidth, &logicalHeight);
        glfwGetFramebufferSize(m_window, &pixelWidth, &pixelHeight);
        // GLFW sizes windows in screen coordinates, which can differ from framebuffer pixels.
        const double scaleX = logicalWidth > 0 && pixelWidth > 0 ? double(pixelWidth) / logicalWidth : 1.0;
        const double scaleY = logicalHeight > 0 && pixelHeight > 0 ? double(pixelHeight) / logicalHeight : 1.0;
        glfwSetWindowSize(m_window, std::max(1, int(std::lround(requested.width / scaleX))),
                          std::max(1, int(std::lround(requested.height / scaleY))));
    }
    m_scrollDelta = m_scroll;
    m_scroll = glm::vec2(0);

    double cursorX = 0, cursorY = 0;
    glfwGetCursorPos(m_window, &cursorX, &cursorY);
    const glm::vec2 cursor{float(cursorX), float(cursorY)};
    // The first sample has no previous position to measure against, so it reports no movement.
    m_cursorDelta = m_hasCursor ? cursor - m_cursor : glm::vec2(0);
    m_cursor = cursor;
    m_hasCursor = true;

    m_leftButton = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    m_rightButton = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (!captureKeyboard && glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    const ImageSize size{uint32_t(width), uint32_t(height)};
    if (size != m_size)
        reconfigure(size);

    return !glfwWindowShouldClose(m_window);
}

ComPtr<ITexture> Window::acquireImage()
{
    if (!m_surface->getConfig())
        return nullptr;

    ComPtr<ITexture> image;
    if (SLANG_FAILED(m_surface->acquireNextImage(image.writeRef())))
        return nullptr;
    return image;
}

void Window::present()
{
    checkRhi(m_surface->present(), "present surface");
}

void Window::requestFramebufferSize(ImageSize size)
{
    if (!size.width || !size.height || size.width > 4096 || size.height > 4096)
        throw std::invalid_argument("Window dimensions must be between 1 and 4096 pixels");
    m_requestedSize = size;
}

void Window::setTitle(const std::string& title)
{
    glfwSetWindowTitle(m_window, title.c_str());
}

} // namespace snr
