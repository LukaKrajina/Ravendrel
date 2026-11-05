#pragma once
// Fox backend/vk_backend.hpp - Fox 的 Vulkan 渲染器
//
// Fox 自己的渲染器。Nuklear 后端只能画填充矩形和文字；这里逐片段求值
// 有向距离场，一条管线就能画出圆角矩形、边框、渐变、软阴影和磨砂玻璃。
//
// 帧结构：
//   * Glass 按控件选用，所以常见情况是单个 render pass 直接画到 swapchain。
//   * 帧里含 glass 命令时，先把所有内容画到离屏 "scene" 图像，4 倍盒式
//     降采样（降采样本身就是模糊），再把 glass 着色器的背景采样器指向
//     那张小图。
//
// 只复用 GLFW 做窗口/surface；Vulkan 状态全部自己持有。

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

        /// 上传有改动的字形图集，然后渲染本帧显示列表。
        void render(const DisplayList &dl, Context &ctx);

        /// 把 GLFW 事件泵入 context 的输入状态；处理窗口尺寸变化。
        void poll_events(Context &ctx);
        /// 窗口已挂接时为 true。
        ///
        /// 没有窗口的后端是 HEADLESS（无头）模式：离屏渲染，永远不会被要求
        /// 关闭。"没有窗口"是一种模式，不是错误。
        bool has_window() const { return window_ != nullptr; }

        /// 窗口请求关闭（GLFW 自身的标志）。
        bool window_close_requested() const
        {
            return window_ != nullptr && glfwWindowShouldClose(window_);
        }

        /// 后端就绪（device 和 instance 都已创建）。
        bool ready() const
        {
            return device_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE;
        }

        // ── 诊断（demo 的统计浮层用）─────────────────────────
        u32 last_instance_count() const { return last_instances_; }
        bool used_blur_path() const { return last_used_blur_; }

        // ── 帧健康 ─────────────────────────────────────────────────
        //
        // 宿主的运行时回退决策全靠这几个数字："有没有帧没能上屏"没法从
        // Fox 暴露的其他任何东西推断，而渲染失败但无声无息，看起来和
        // UI 空白完全一样。
        u32 frame_faults() const { return frame_faults_; }
        const char *last_fault() const { return last_fault_; }
        u32 last_dropped() const { return last_dropped_; }

        /// 驱动上报的 GPU 身份。create_device 之前为空 / 0。
        ///
        /// 宿主按 GPU 维度存策略（tile GPU 与立即模式 GPU 想要的默认值不同——
        /// 见 utils/A2a.hpp），而"这是哪块 GPU"只能问驱动，绝不能靠猜。
        const char *gpu_name() const { return gpu_name_; }
        u32 gpu_vendor() const { return gpu_vendor_; }

        /// 回读已呈现的 swapchain 图像，写成二进制 PPM。
        ///
        /// 窗口截图可能被遮挡、位置不对或抓在过时的时刻，用来回答"GPU 真的
        /// 画了这个吗"没有意义。这里从显存里把真实图像拷出来，结果就是事实。
        /// 只有拷贝本身失败才返回 false。
        bool dump_ppm(const char *path);

    private:
        // 每个 FontId 一个字形图集。声明在返回其指针的方法之前，
        // 保证每次使用时类型都完整。
        struct FontSlot
        {
            FontId id = FONT_INVALID;
            VkImage img = VK_NULL_HANDLE;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            size_t uploaded_glyphs = 0; ///< 上次上传时的图集字形数
            bool uploaded = false;
        };
        std::vector<FontSlot> font_slots_;

        // 每个 TexId 一个纹理槽。结构与 FontSlot 相同，但上传生命周期不同：
        // 字形图集每次只长几个字形，应用纹理则整张写入、很少更新。
        struct TexSlot
        {
            TexId id = TEX_NONE;
            VkImage img = VK_NULL_HANDLE;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            u32 w = 0, h = 0;
            u32 generation = 0; ///< 该槽持有的 Context 代际号
            bool uploaded = false;
            /// 创建图像时的 mip 级数。Context 一侧的纹理金字塔增减
            /// （create_texture 与 create_texture_mipped 之间切换）时必须
            /// 重建图像，不能重新上传进级数不对的图像——那样拷贝会越出
            /// 第 0 级的末尾，或让上层 mip 保持未定义。
            u32 mip_levels = 1;
        };
        std::vector<TexSlot> tex_slots_;
        /// 描述符集上次写入时对应的 Context 代际号。
        u32 tex_desc_generation_ = 0;

        // 1x1 占位图，让描述符 binding 1 和 2 在任何字形图集或模糊目标
        // 存在之前都有有效图像。Vulkan 禁止往 COMBINED_IMAGE_SAMPLER 写
        // 空 imageView，也禁止在 binding 未写入时绘制——占位图从第一帧起
        // 就同时满足这两个要求。
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
        /// 把描述符集数组重分配为恰好
        /// 1 + font_slots_.size() + tex_slots_.size() 个集合，按这个固定
        /// 顺序。这个顺序就是 write_descriptors() 和绘制循环共同依赖的
        /// 约定，往 vector 后面追加会破坏它。
        bool rebuild_desc_sets();
        FontId slot_of(FontId f) const;
        FontSlot *slot(FontId f);
        int ensure_slot(FontId f);
        int ensure_tex_slot(TexId t);
        /// 已创建槽位的下标，没有则返回 -1。绝不创建：绘制路径在按槽位
        /// 构建实例期间不能改动槽位 vector。
        int tex_slot_index(TexId t) const;
        bool ensure_offscreen();
        /// 创建图像、它的 device-local 内存和一个 view。任何失败都返回 false
        /// 并把三者留空——为什么这里不检查失败会在很晚之后把堆搞坏，见 .cpp
        /// 里的注释。
        bool make_image(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImage &img,
                        VkDeviceMemory &mem, VkImageView &view, u32 mip_levels = 1);
        void destroy_offscreen();
        void record_blur_pass(VkCommandBuffer cmd);
        /// 记录一次出错的帧：累加计数、保留原因，并且每次出错都喊一声
        /// （没人看见的错误正是 rvdl 回退要排除的故障形态）。
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
        /// 单个可复用的帧命令缓冲。分配一次、每帧 reset；只在 shutdown()
        /// 里释放。GPU 还在执行它时按帧释放违反规范。
        VkCommandBuffer frame_cmd_ = VK_NULL_HANDLE;

        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        std::vector<VkImage> swap_images_;
        std::vector<VkImageView> swap_views_;
        std::vector<VkFramebuffer> swap_fb_;
        VkFormat color_format_ = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D extent_{};
        VkSemaphore sem_available_ = VK_NULL_HANDLE;
        /// 每个 swapchain 图像一个 render-finished 信号量。共用一个会与
        /// 呈现引擎竞争（VUID-vkQueueSubmit-pSignalSemaphores-00067）：
        /// 图像 N 的 present 可能还在读帧缓冲时，帧 N+1 就已经给它发信号。
        std::vector<VkSemaphore> sem_render_;
        VkFence fence_ = VK_NULL_HANDLE;

        // 主 SDF+文字管线。
        //
        // 是两个 render pass，不是一个。render pass 要声明附件的 initialLayout
        // 和 finalLayout，两个目标的声明本来就不同：
        //   · swapchain：进来时是 PRESENT_SRC_KHR，结束时也必须是它才能呈现
        //   · offscreen：创建为 UNDEFINED，结束时必须是 COLOR_ATTACHMENT_OPTIMAL
        //                 让 record_blur_pass 能把它转到 TRANSFER_SRC
        // 共用一个 pass 会让其中一个声明变成假话，校验层会把
        // VUID-VkImageMemoryBarrier-oldLayout-01197 每一帧都报一遍。
        VkRenderPass render_pass_ = VK_NULL_HANDLE;      ///< -> swapchain
        VkRenderPass scene_pass_ = VK_NULL_HANDLE;       ///< -> 离屏 scene
        VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
        VkPipeline pipe_ = VK_NULL_HANDLE;
        VkPipeline scene_pipe_ = VK_NULL_HANDLE;

        // 几何：单位四边形 + 实例缓冲（host-visible，常驻映射）
        VkBuffer vertex_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory vertex_mem_ = VK_NULL_HANDLE;
        VkBuffer instance_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory instance_mem_ = VK_NULL_HANDLE;
        void *instance_mapped_ = nullptr;
        u32 instance_capacity_ = 0;

        // 字形图集声明在上面的其他私有成员里。
        VkSampler sampler_ = VK_NULL_HANDLE;

        // 描述符。每个字体槽一个描述符 SET，共享同一个 UBO 和模糊图像；
        // 只有 binding 1（字形图集）不同。绘制时绑定与批次字体匹配的集合。
        VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> desc_sets_;
        VkBuffer ubo_ = VK_NULL_HANDLE;
        VkDeviceMemory ubo_mem_ = VK_NULL_HANDLE;
        void *ubo_mapped_ = nullptr;

        // 离屏 scene + 4 倍降采样的模糊目标（glass 背景）
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
        /// blur_img_ 至少被转到过一次 SHADER_READ_ONLY 后为 true。
        /// 与 blur_layout_ 并存，方便只需要"这张图像写过没有"的调用点。
        bool blur_img_created_ = false;
        /// 两张离屏图像当前确切的 layout。
        ///
        /// 要跟踪它们，是因为描述符只允许在图像处于 SHADER_READ_ONLY_OPTIMAL
        /// 时引用它，而两张图在一帧内都会经过好几个别的 layout。瞎猜任何一个
        /// 就是 VUID-vkCmdDraw-None-09600 或 VUID-VkImageMemoryBarrier-oldLayout。
        VkImageLayout blur_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout scene_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        /// 由 record_blur_pass 置位，在下一次 render() 开头处理。
        /// 描述符更新不能在录制中途进行。
        bool blur_binding_stale_ = false;

        u32 frame_index_ = 0;
        u32 last_instances_ = 0;
        bool last_used_blur_ = false;
        /// 累计没能上屏的帧数、最近一次的原因，以及上一帧因空间不足丢弃的
        /// 几何量。宿主为什么需要它们，见上面的访问接口。
        u32 frame_faults_ = 0;
        const char *last_fault_ = "";
        u32 last_dropped_ = 0;
        /// 驱动上报的 GPU 身份（见 gpu_name()/gpu_vendor()）。
        char gpu_name_[256] = {};
        u32 gpu_vendor_ = 0;
        /// 模糊目标的降采样除数，按当前质量档选择（4 = 全质量，8 = 半质量）。
        /// 存下来是因为档位变化时需要重建。
        u32 blur_div_ = 4;
        /// 最近一次 dump_ppm 中与帧角颜色不同的像素数。健康的 UI 是几万
        /// 量级；0 表示 GPU 什么都没画，不管窗口看起来什么样。
        size_t best_pixels_ = 0;

        /// 本帧转换出的几何。由共享的 draw/instance.cpp 层填充；`batches_`
        /// 在渲染时留一份本地副本，因为绘制循环需要稳定的视图。
        ///
        /// InstanceData、Batch 和转换都放在 draw/instance.hpp 里，CPU 后端
        /// 消费的是完全相同的数据。两个后端各做各的转换会走形，而且走形
        /// 看起来像"CPU 渲染器有点不对"，而不是"在一处修好了、另一处
        /// 坏了"。
        InstanceSet frame_;
        std::vector<Batch> batches_;

        static double g_scroll_x_, g_scroll_y_;
        static std::vector<u32> g_typed_chars;
    };
}