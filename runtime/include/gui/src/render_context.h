#pragma once
#include <vector>
#include <string>

#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"
#include "nuklear_config.h"

namespace qgui
{

    class RenderContext
    {
    public:
        RenderContext() = default;
        ~RenderContext() { shutdown(); }

        RenderContext(const RenderContext &) = delete;
        RenderContext &operator=(const RenderContext &) = delete;

        bool init(const char *title, int width, int height);
        void shutdown();

        bool should_close() const;
        void begin_frame();
        void end_frame();

        nk_context *ctx() const { return ctx_; }
        float delta_time() const { return delta_time_; }
        VkPresentModeKHR present_mode() const { return present_mode_; }
        GLFWwindow *window() const { return window_; }

    private:
        bool create_swapchain();
        void destroy_swapchain();

        GLFWwindow *window_ = nullptr;
        VkInstance instance_ = VK_NULL_HANDLE;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue queue_ = VK_NULL_HANDLE;
        uint32_t queue_family_ = 0;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        std::vector<VkImage> swapchain_images_;
        std::vector<VkImageView> swapchain_image_views_;
        VkFormat color_format_ = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D extent_{};
        VkSemaphore image_available_ = VK_NULL_HANDLE;
        VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

        nk_context *ctx_ = nullptr;
        float delta_time_ = 0.0f;
        double last_time_ = 0.0;
    };
}
