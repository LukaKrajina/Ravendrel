#pragma once
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cstring>

#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"

namespace rvdlrsp::render {

    constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    class VulkanContext {
    public:
        VulkanContext() = default;
        ~VulkanContext() { shutdown(); }

        VulkanContext(const VulkanContext &) = delete;
        VulkanContext &operator=(const VulkanContext &) = delete;

        bool init(const char *title, int width, int height);
        void shutdown();

        GLFWwindow *window() const { return window_; }
        VkDevice device() const { return device_; }
        VkPhysicalDevice physical_device() const { return physical_device_; }
        VkQueue graphics_queue() const { return queue_; }
        uint32_t queue_family() const { return queue_family_; }
        VkRenderPass render_pass() const { return render_pass_; }
        VkExtent2D extent() const { return extent_; }
        uint32_t image_count() const { return static_cast<uint32_t>(swapchain_images_.size()); }
        VkFormat color_format() const { return color_format_; }
        VkFramebuffer framebuffer(uint32_t index) const { return framebuffers_[index]; }

        bool should_close() const { return window_ == nullptr || glfwWindowShouldClose(window_); }
        void poll_events() const { glfwPollEvents(); }

        // 开始一帧：acquire image，返回该帧的 framebuffer 与 command buffer
        VkCommandBuffer begin_frame(uint32_t &out_image_index);
        // 结束一帧：submit + present
        void end_frame(uint32_t image_index, VkCommandBuffer cmd);

    private:
        bool create_swapchain();
        void create_depth_resources();
        void create_render_pass();
        void create_framebuffers();
        void create_command_buffers();
        void create_sync_objects();
        void destroy_swapchain();
        void destroy_depth_resources();
        void destroy_framebuffers();

        uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);

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

        VkImage depth_image_ = VK_NULL_HANDLE;
        VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
        VkImageView depth_view_ = VK_NULL_HANDLE;

        VkRenderPass render_pass_ = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> framebuffers_;

        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> command_buffers_;

        std::vector<VkSemaphore> image_available_;
        std::vector<VkSemaphore> render_finished_;
        std::vector<VkFence> in_flight_fences_;
        uint32_t current_frame_ = 0;
    };
}