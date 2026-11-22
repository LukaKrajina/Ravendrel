#pragma once

#include "core/context.hpp"
#include "draw/display_list.hpp"
#include "draw/instance.hpp"
#include "text/font.hpp"

#define GLFW_INCLUDE_VULKAN
#include "glfw3.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace fox
{

    class VkBackend
    {
    public:
        bool init(GLFWwindow *window, Context &ctx);
        void shutdown();

        void render(const DisplayList &dl, Context &ctx);

        void poll_events(Context &ctx);
        bool has_window() const { return window_ != nullptr; }

        bool window_close_requested() const
        {
            return window_ != nullptr && glfwWindowShouldClose(window_);
        }

        bool ready() const
        {
            return device_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE;
        }

        u32 last_instance_count() const { return last_instances_; }
        bool used_blur_path() const { return last_used_blur_; }

        u32 frame_faults() const { return frame_faults_; }
        const char *last_fault() const { return last_fault_; }
        u32 last_dropped() const { return last_dropped_; }

        const char *gpu_name() const { return gpu_name_; }
        u32 gpu_vendor() const { return gpu_vendor_; }

        bool dump_ppm(const char *path);

    private:
        struct FontSlot
        {
            FontId id = FONT_INVALID;
            VkImage img = VK_NULL_HANDLE;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            size_t uploaded_glyphs = 0;
            bool uploaded = false;
        };
        std::vector<FontSlot> font_slots_;

        struct TexSlot
        {
            TexId id = TEX_NONE;
            VkImage img = VK_NULL_HANDLE;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            u32 w = 0, h = 0;
            u32 generation = 0;
            bool uploaded = false;
            u32 mip_levels = 1;
        };
        std::vector<TexSlot> tex_slots_;
        u32 tex_desc_generation_ = 0;

        VkImage dummy_img_ = VK_NULL_HANDLE;
        VkDeviceMemory dummy_mem_ = VK_NULL_HANDLE;
        VkImageView dummy_view_ = VK_NULL_HANDLE;

        u32 find_mem_type(u32 bits, VkMemoryPropertyFlags props);
        float content_scale() const;
        bool create_instance();
        bool create_device();
        bool create_swapchain();
        void destroy_swapchain();
        bool create_geometry();
        bool create_pipeline();
        void ensure_framebuffers();
        bool create_descriptors();
        bool create_dummy_image();
        bool create_sampler();
        bool upload_atlas(FontId id);
        bool upload_texture(const Context::Texture &src, TexSlot &ts);
        void sync_textures();
        void write_descriptors();
        bool rebuild_desc_sets();
        FontId slot_of(FontId f) const;
        FontSlot *slot(FontId f);
        int ensure_slot(FontId f);
        int ensure_tex_slot(TexId t);
        int tex_slot_index(TexId t) const;
        bool ensure_offscreen();
        bool make_image(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImage &img,
                        VkDeviceMemory &mem, VkImageView &view, u32 mip_levels = 1);
        void destroy_offscreen();
        void record_blur_pass(VkCommandBuffer cmd);
        void fault(const char *why);

        GLFWwindow *window_ = nullptr;
        Context *ctx_ = nullptr;

        VkInstance instance_ = VK_NULL_HANDLE;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkPhysicalDevice phys_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue queue_ = VK_NULL_HANDLE;
        u32 queue_family_ = 0;
        VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
        VkCommandBuffer frame_cmd_ = VK_NULL_HANDLE;

        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        std::vector<VkImage> swap_images_;
        std::vector<VkImageView> swap_views_;
        std::vector<VkFramebuffer> swap_fb_;
        VkFormat color_format_ = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D extent_{};
        VkSemaphore sem_available_ = VK_NULL_HANDLE;
        std::vector<VkSemaphore> sem_render_;
        VkFence fence_ = VK_NULL_HANDLE;

        VkRenderPass render_pass_ = VK_NULL_HANDLE;
        VkRenderPass scene_pass_ = VK_NULL_HANDLE;
        VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
        VkPipeline pipe_ = VK_NULL_HANDLE;
        VkPipeline scene_pipe_ = VK_NULL_HANDLE;

        VkBuffer vertex_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory vertex_mem_ = VK_NULL_HANDLE;
        VkBuffer instance_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory instance_mem_ = VK_NULL_HANDLE;
        void *instance_mapped_ = nullptr;
        u32 instance_capacity_ = 0;

        VkSampler sampler_ = VK_NULL_HANDLE;

        VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> desc_sets_;
        VkBuffer ubo_ = VK_NULL_HANDLE;
        VkDeviceMemory ubo_mem_ = VK_NULL_HANDLE;
        void *ubo_mapped_ = nullptr;

        VkImage scene_img_ = VK_NULL_HANDLE;
        VkDeviceMemory scene_mem_ = VK_NULL_HANDLE;
        VkImageView scene_view_ = VK_NULL_HANDLE;
        VkImage blur_img_ = VK_NULL_HANDLE;
        VkDeviceMemory blur_mem_ = VK_NULL_HANDLE;
        VkImageView blur_view_ = VK_NULL_HANDLE;

        VkFramebuffer blur_fb_ = VK_NULL_HANDLE;
        VkFramebuffer scene_fb_ = VK_NULL_HANDLE;
        u32 blur_w_ = 0;
        u32 blur_h_ = 0;
        bool blur_img_created_ = false;
        VkImageLayout blur_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout scene_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        bool blur_binding_stale_ = false;

        u32 frame_index_ = 0;
        u32 last_instances_ = 0;
        bool last_used_blur_ = false;
        u32 frame_faults_ = 0;
        const char *last_fault_ = "";
        u32 last_dropped_ = 0;
        char gpu_name_[256] = {};
        u32 gpu_vendor_ = 0;
        u32 blur_div_ = 4;
        size_t best_pixels_ = 0;

        InstanceSet frame_;
        std::vector<Batch> batches_;

        static double g_scroll_x_, g_scroll_y_;
        static std::vector<u32> g_typed_chars;
    };
}