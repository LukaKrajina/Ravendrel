// Fox backend/vk_backend.cpp - Vulkan 渲染器实现
//
// 刻意做成单个编译单元：管线、描述符和帧循环代码耦合很紧（每个 pass 共享
// 同一批 layout），拆开只会把耦合藏到头文件后面。

#include "vk_backend.hpp"
#include "shaders_spv.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace fox
{
    /// 把一个实例从**逻辑单位**换算到**设备像素**（DPI 缩放）。
    ///
    /// 需要缩放的是**长度**量：矩形、圆角、描边粗细、阴影模糊，以及
    /// path / triangle 的线段长度、端帽与顶点偏移 —— 它们都以逻辑像素为单位。
    /// 方向、比例、颜色、UV、标志位不是长度，不动。
    ///
    /// ⚠ params 的含义**随图元类型变**（见 instance.hpp 的字段说明）：
    ///   params.z  = 阴影模糊（图元）/ 线段长度（path）
    ///   params.w  = 方向 x（path）/ 顶点 2 的 x（triangle）
    ///   params2.x = 玻璃饱和度（图元）/ 端帽（path）/ 顶点 2 的 y（triangle）
    /// 所以必须按类型分别处理 —— 统一「把所有 params 都乘一遍」会把方向向量
    /// 与饱和度也放大，线段的方向就歪了。
    static void scale_instance(InstanceData &d, f32 s)
    {
        for (int i = 0; i < 4; ++i)
        {
            d.rect[i] *= s;
            d.radii[i] *= s;
        }
        d.params[1] *= s;   // 描边粗细（所有图元都是长度）
        d.params[2] *= s;   // 阴影模糊（图元）/ 线段长度（path）—— 都是长度

        const f32 type = d.params[0];
        if (type == PT_PATH)
        {
            // params[3] = 方向 x（不是长度）；params2.x = 端帽、
            // params2.y = 拼接 —— 后两个是长度。
            d.params2[0] *= s;
            d.params2[1] *= s;
        }
        else if (type == PT_TRIANGLE)
        {
            // 三个顶点相对矩形中心：params[1..3] 与 params2[0..2]
            // （params[1]、params[2] 上面已按通用规则缩放过了）。
            d.params[3]  *= s;
            d.params2[0] *= s;
            d.params2[1] *= s;
            d.params2[2] *= s;
        }
    }


    // 图元类型 id 现在在 draw/instance.hpp 里，与 CPU 光栅化器共用。
    // 它们必须与 shaders.frag 里的 PTYPE_* 常量一致。

    /// FOX_SCALE 覆盖值，0 表示问窗口系统。定义在下面的几何辅助函数那里。
    static float backend_scale();

    double VkBackend::g_scroll_x_ = 0.0;
    double VkBackend::g_scroll_y_ = 0.0;
    std::vector<u32> VkBackend::g_typed_chars;

    // ── 辅助 ────────────────────────────────────────────────────────

    // 做成成员（而非自由函数），就能直接访问 phys_，不必每个调用点都多传
    // 一个参数——setup 路径里到处都要查内存类型。
    u32 VkBackend::find_mem_type(u32 bits, VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        for (u32 i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        return UINT32_MAX;
    }

    // ── init / shutdown ────────────────────────────────────────────────

    bool VkBackend::init(GLFWwindow *window, Context &ctx)
    {
        window_ = window;
        ctx_ = &ctx;
        try
        {
            // 每一步自己报告失败原因。否则每步都只返回 false，调用方只能说
            // "init failed"，而真正原因是"你的 GPU 挑了一个管线用不了的格式"
            // 时这毫无帮助。
            if (!create_instance()) { std::fprintf(stderr, "[Fox] create_instance failed\n"); return false; }
            std::fprintf(stderr, "[Fox] instance ok\n");
            if (!create_device())   { std::fprintf(stderr, "[Fox] create_device failed\n"); return false; }
            std::fprintf(stderr, "[Fox] device ok\n");

            int w = 960, h = 640;
            glfwGetFramebufferSize(window_, &w, &h);
            if (w <= 0) w = 960;
            if (h <= 0) h = 640;

            // FOX_SCALE 强制指定密度，这是在 1x 开发机上测 HiDPI 路径的唯一
            // 办法——而 bug 恰恰藏在 1x 机器上，因为那里每个数字看起来都对。
            float sc = backend_scale();
            if (sc > 0.0f) ctx_->set_scale(sc);
            ctx_->set_framebuffer_size(w, h);

            if (!create_swapchain()) { std::fprintf(stderr, "[Fox] create_swapchain failed\n"); return false; }
            if (!create_geometry())  { std::fprintf(stderr, "[Fox] create_geometry failed\n"); return false; }
            std::fprintf(stderr, "[Fox] geometry ok\n");
            if (!create_pipeline())  { std::fprintf(stderr, "[Fox] create_pipeline failed\n"); return false; }
            std::fprintf(stderr, "[Fox] pipeline ok\n");
            // ⚠ Sampler 必须在描述符之前创建。create_descriptors() 会触发
            //   描述符写入（经由 upload_atlas），用空 sampler 写
            //   COMBINED_IMAGE_SAMPLER 会绑上垃圾，驱动可能解析到不相关的
            //   图像——表现就是"u_blurred 指向了 swapchain 图像"。
            if (!create_sampler())   { std::fprintf(stderr, "[Fox] create_sampler failed\n"); return false; }
            if (!create_descriptors()){ std::fprintf(stderr, "[Fox] create_descriptors failed\n"); return false; }

            glfwSetScrollCallback(window_, [](GLFWwindow *, double x, double y) {
                g_scroll_x_ += x;
                g_scroll_y_ += y;
            });
            // Unicode 码点：这是拿到 IME/CJK 输入（数字、标点和任何非 ASCII
            // 字符）的唯一途径。
            glfwSetCharCallback(window_, [](GLFWwindow *, unsigned int cp) {
                if (cp >= 32 && cp != 127) g_typed_chars.push_back(cp);
            });

            FontId f = ctx_->font(ctx_->theme().font_body);
            if (f != FONT_INVALID && !upload_atlas(f))
            {
                std::fprintf(stderr, "[Fox] upload_atlas failed\n");
                return false;
            }
            std::fprintf(stderr, "[Fox] backend ready (%dx%d)\n", (int)extent_.width,
                         (int)extent_.height);
            return true;
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[Fox] %s\n", e.what());
            return false;
        }
    }

    bool VkBackend::create_instance()
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Fox";
        app.apiVersion = VK_API_VERSION_1_0;

        u32 n = 0;
        const char **exts = glfwGetRequiredInstanceExtensions(&n);
        // 按需开启校验：环境变量 FOX_VALIDATE=1。否则 vkCreateGraphicsPipelines
        // 里的驱动崩溃无从归因，而校验层会在崩溃前打印确切的出错状态。
        bool validate = getenv("FOX_VALIDATE") != nullptr;
        std::vector<const char *> inst_exts;
        for (u32 i = 0; i < n; ++i) inst_exts.push_back(exts[i]);
        if (validate) inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = (u32)inst_exts.size();
        ci.ppEnabledExtensionNames = inst_exts.data();
        if (validate)
        {
            u32 lc = 0;
            vkEnumerateInstanceLayerProperties(&lc, nullptr);
            std::vector<VkLayerProperties> layers(lc);
            vkEnumerateInstanceLayerProperties(&lc, layers.data());
            bool have = false;
            for (auto &l : layers)
                if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                    have = true;
            if (have)
            {
                const char *ln = "VK_LAYER_KHRONOS_validation";
                ci.enabledLayerCount = 1;
                ci.ppEnabledLayerNames = &ln;
                std::fprintf(stderr, "[Fox] validation layer ENABLED\n");
            }
            else
            {
                std::fprintf(stderr, "[Fox] FOX_VALIDATE set but layer not installed\n");
            }
        }

        if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) return false;
        if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
            return false;

        u32 count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (!count) return false;
        std::vector<VkPhysicalDevice> devs(count);
        vkEnumeratePhysicalDevices(instance_, &count, devs.data());
        auto score = [](VkPhysicalDeviceType t) {
            switch (t)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
            default: return 0;
            }
        };
        int best = -1;
        for (u32 i = 0; i < count; ++i)
        {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devs[i], &p);
            int sc = score(p.deviceType);
            if (sc > best)
            {
                best = sc;
                phys_ = devs[i];
            }
        }
        VkPhysicalDeviceProperties dp{};
        vkGetPhysicalDeviceProperties(phys_, &dp);
        std::fprintf(stderr, "[Fox] GPU: %s\n", dp.deviceName);
        // 保留而不只是打印：宿主把它上报给散热/质量策略层（utils/A2a.hpp），
        // 在那里再查一次会得到同一问题的第二个答案。
        std::snprintf(gpu_name_, sizeof(gpu_name_), "%s", dp.deviceName);
        gpu_vendor_ = dp.vendorID;

        u32 qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qc, qf.data());
        bool found_family = false;
        for (u32 i = 0; i < qc; ++i)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                queue_family_ = i;
                found_family = true;
                break;
            }
        std::fprintf(stderr, "[Fox] queue families=%u graphics=%u\n", qc, (unsigned)found_family);

        // 图形队列族还必须能向这个 surface 呈现。建出不能呈现的 device 会在
        // 很久之后（vkQueuePresentKHR 时）报一个晦涩的错误，所以在这里查，
        // 能说清是哪里的问题。
        VkBool32 present_ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &present_ok);
        std::fprintf(stderr, "[Fox] surface present support=%d\n", (int)present_ok);
        if (!present_ok)
        {
            std::fprintf(stderr, "[Fox] chosen queue family cannot present\n");
            return false;
        }
        return true;
    }

    bool VkBackend::create_device()
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queue_family_;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;

        const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = dext;
        if (vkCreateDevice(phys_, &ci, nullptr, &device_) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = queue_family_;
        vkCreateCommandPool(device_, &pi, nullptr, &cmd_pool_);
        return true;
    }

    void VkBackend::shutdown()
    {
        // ⚠ GLFW 窗口必须在本调用**之后**销毁。
        //
        //   glfwCreateWindowSurface 把 VkSurfaceKHR 的所有权交给应用，
        //   Vulkan 要求 surface 必须在仍然有效时被销毁——所以 surface（以及
        //   创建它用的 instance）必须先走，然后才是窗口。
        //
        //   这里原来的注释说反了（"调用前必须先销毁窗口"），直接和本文件
        //   下方的说明（以及 fox.h 的 TEARDOWN ORDER）矛盾。
        if (device_)
        {
            vkDeviceWaitIdle(device_);
            destroy_offscreen();
            destroy_swapchain();
            if (instance_mapped_) vkUnmapMemory(device_, instance_mem_);
            if (instance_buf_) vkDestroyBuffer(device_, instance_buf_, nullptr);
            if (instance_mem_) vkFreeMemory(device_, instance_mem_, nullptr);
            if (vertex_buf_) vkDestroyBuffer(device_, vertex_buf_, nullptr);
            if (vertex_mem_) vkFreeMemory(device_, vertex_mem_, nullptr);
            for (auto &fs : font_slots_)
            {
                if (fs.view) vkDestroyImageView(device_, fs.view, nullptr);
                if (fs.img) vkDestroyImage(device_, fs.img, nullptr);
                if (fs.mem) vkFreeMemory(device_, fs.mem, nullptr);
                fs = FontSlot{};
            }
            font_slots_.clear();

            // ⚠ 描述符池先销毁，在它引用的任何图像、view、sampler 或内存
            //   被销毁之前。
            //
            //   先销毁图像而描述符集还引用着它们，集合就悬空了；规范容忍这种
            //   情况，但实测发现有些驱动在销毁池时会遍历这些悬空条目——症状是
            //   vkDestroyDevice 内部崩溃，没有校验消息，而且发生在真正肇祸的
            //   那一帧之后好几帧。
            //
            //   先拆池就不存在顺序问题了：图像走的时候已经没人引用它们。
            if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
            desc_pool_ = VK_NULL_HANDLE;
            desc_sets_.clear();

            for (TexSlot &t : tex_slots_)
            {
                if (t.view) vkDestroyImageView(device_, t.view, nullptr);
                if (t.img) vkDestroyImage(device_, t.img, nullptr);
                if (t.mem) vkFreeMemory(device_, t.mem, nullptr);
            }
            tex_slots_.clear();
            if (dummy_view_) vkDestroyImageView(device_, dummy_view_, nullptr);
            if (dummy_img_) vkDestroyImage(device_, dummy_img_, nullptr);
            if (dummy_mem_) vkFreeMemory(device_, dummy_mem_, nullptr);
            dummy_view_ = VK_NULL_HANDLE;
            dummy_img_ = VK_NULL_HANDLE;
            dummy_mem_ = VK_NULL_HANDLE;
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
            if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            set_layout_ = VK_NULL_HANDLE;
            if (ubo_) vkDestroyBuffer(device_, ubo_, nullptr);
            if (ubo_mem_) vkFreeMemory(device_, ubo_mem_, nullptr);
            vkDeviceWaitIdle(device_);
            if (frame_cmd_) vkFreeCommandBuffers(device_, cmd_pool_, 1, &frame_cmd_);
            if (pipe_) vkDestroyPipeline(device_, pipe_, nullptr);
            if (scene_pipe_) vkDestroyPipeline(device_, scene_pipe_, nullptr);
            if (pipe_layout_) vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
            if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
            if (scene_pass_) vkDestroyRenderPass(device_, scene_pass_, nullptr);
            if (cmd_pool_) vkDestroyCommandPool(device_, cmd_pool_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }

        // 即使 device 没建成，instance / surface 也可能已经存在 ——
        // 「设备创建失败」那条路径正是如此（create_device 失败 → init 返回
        // false → 上层调 shutdown）。之前这里 `if (!device_) return;` 直接
        // 返回，把刚建好的 instance/surface 永远漏掉 —— 而「驱动缺失 →
        // 回退 Nuklear」恰恰是最常走这条路的场景。
        if (surface_)
        {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_)
        {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    // ── swapchain ───────────────────────────────────────────────────────

    bool VkBackend::create_swapchain()
    {
        std::fprintf(stderr, "[Fox] swapchain: caps...\n");
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
        // ⚠ currentExtent == UINT32_MAX 的语义是「**尺寸由你定**」（surface 的
        // 大小由你创建的 swapchain 决定），不是「真的是 40 亿像素」。
        // 直接把它当字面值用会申请一块 4294967295×4294967295 的 swapchain，
        // vkCreateSwapchainKHR 随即以 VK_ERROR_OUT_OF_DEVICE_MEMORY(-2) 失败，
        // 表现为「窗口起不来 / 渲染器无后端」。
        //
        // 这是驱动无关的：凡是报告 currentExtent = UINT32_MAX 的 surface
        // （Wayland / XWayland 常见）都会踩到。正确做法是取窗口当前尺寸。
        if (caps.currentExtent.width != UINT32_MAX)
        {
            extent_.width  = caps.currentExtent.width;
            extent_.height = caps.currentExtent.height;
        }
        else
        {
            int w = 960, h = 640;
            glfwGetFramebufferSize(window_, &w, &h);
            if (w <= 0) w = 960;
            if (h <= 0) h = 640;
            extent_.width  = (u32)w;
            extent_.height = (u32)h;
            // 夹到 surface 声明的支持范围内
            if (caps.maxImageExtent.width  != UINT32_MAX && extent_.width  > caps.maxImageExtent.width)
                extent_.width  = caps.maxImageExtent.width;
            if (caps.maxImageExtent.height != UINT32_MAX && extent_.height > caps.maxImageExtent.height)
                extent_.height = caps.maxImageExtent.height;
            if (extent_.width  < caps.minImageExtent.width)  extent_.width  = caps.minImageExtent.width;
            if (extent_.height < caps.minImageExtent.height) extent_.height = caps.minImageExtent.height;
        }
        std::fprintf(stderr, "[Fox] swapchain: extent %ux%u\n", extent_.width, extent_.height);

        u32 mc = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &mc, nullptr);
        std::vector<VkPresentModeKHR> modes(mc);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &mc, modes.data());
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                pm = m;
                break;
            }
        std::fprintf(stderr, "[Fox] swapchain: %u present modes\n", mc);

        u32 fc = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        if (fc) vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, fmts.data());
        color_format_ = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;
        std::fprintf(stderr, "[Fox] swapchain: %u surface formats, using fmt=%d\n", fc,
                     (int)color_format_);

        u32 img_count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
            img_count = caps.maxImageCount;
        std::fprintf(stderr, "[Fox] swapchain: images=%u (min=%u max=%u)\n", img_count,
                     caps.minImageCount, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = img_count;
        ci.imageFormat = color_format_;
        ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent = extent_;
        ci.imageArrayLayers = 1;
        // TRANSFER_SRC 只为 dump_ppm() 而设——回读像素验证真正到达 swapchain
        // 的内容，不受窗口截图碰巧抓到什么影响。
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = pm;
        ci.clipped = VK_TRUE;
        std::fprintf(stderr, "[Fox] swapchain: vkCreateSwapchainKHR...\n");
        VkResult scr = vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_);
        std::fprintf(stderr, "[Fox] swapchain: created=%d\n", (int)scr);
        if (scr != VK_SUCCESS) return false;

        u32 n = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        swap_images_.resize(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, swap_images_.data());
        swap_views_.resize(n);
        for (u32 i = 0; i < n; ++i)
        {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swap_images_[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = color_format_;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            vkCreateImageView(device_, &vi, nullptr, &swap_views_[i]);
        }

        // Swapchain 帧缓冲依赖 render pass，而它在后面的 create_pipeline()
        // 才创建。所以帧缓冲推迟到第一帧再建（用空的 swap_fb_ vector 判断）。
        // 见 ensure_framebuffers()。

        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &si, nullptr, &sem_available_);

        // 每个 swapchain 图像一个 render-finished 信号量，按我们画入的图像
        // 索引。
        //
        // 单个共享信号量看着简单，却是经典 bug：呈现引擎可能还在读图像 N 的
        // 内容，帧 N+1 就给图像 N+1 发了同一个信号量。这就是
        // VUID-vkQueueSubmit-pSignalSemaphores-00067，表现为间歇性撕裂而不是
        // 报错。按图像分信号量，等待的才是它所属的那张图。
        sem_render_.assign(n, VK_NULL_HANDLE);
        for (u32 i = 0; i < n; ++i) vkCreateSemaphore(device_, &si, nullptr, &sem_render_[i]);
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fi, nullptr, &fence_);
        std::fprintf(stderr, "[Fox] swapchain: %u views, semaphores ok\n", (unsigned)swap_views_.size());
        return true;
    }

    void VkBackend::destroy_swapchain()
    {
        for (auto f : swap_fb_)
            if (f) vkDestroyFramebuffer(device_, f, nullptr);
        swap_fb_.clear();
        for (auto v : swap_views_)
            if (v) vkDestroyImageView(device_, v, nullptr);
        swap_views_.clear();
        swap_images_.clear();
        if (swapchain_)
        {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        if (sem_available_)
        {
            vkDestroySemaphore(device_, sem_available_, nullptr);
            sem_available_ = VK_NULL_HANDLE;
        }
        for (VkSemaphore s : sem_render_)
            if (s) vkDestroySemaphore(device_, s, nullptr);
        sem_render_.clear();
        if (fence_)
        {
            vkDestroyFence(device_, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
        }
    }

    // ── 几何 ───────────────────────────────────────────────────────

    /// 要使用的显示密度，单位是每逻辑像素的物理像素数。
    ///
    /// FOX_SCALE 设置了就以它为准：这是在标准密度开发显示器上测 HiDPI 路径
    /// 的唯一办法——那里每个坐标都"正确"，缩放 bug 因此不可见。
    static float backend_scale()
    {
        if (const char *e = getenv("FOX_SCALE"))
        {
            const float v = (float)atof(e);
            if (v > 0.0f) return v;
        }
        return 0.0f; // 0 = 问窗口系统
    }

    /// 窗口当前显示器的内容缩放。
    float VkBackend::content_scale() const
    {
        const float forced = backend_scale();
        if (forced > 0.0f) return forced;
        float sx = 1.0f, sy = 1.0f;
        if (window_)
        {
            glfwGetWindowContentScale(window_, &sx, &sy);
            if (sx <= 0.0f) sx = 1.0f;
            if (sy <= 0.0f) sy = 1.0f;
        }
        // Fox 只按单轴布局。取平均对各向异性的设置是错的；常见情况是
        // sx == sy，取 X 可避免 UI 悄悄拿到没人要求的小数缩放比。
        return sx;
    }

    bool VkBackend::create_geometry()
    {
        const float quad[] = {
            0, 0, 1, 0, 0, 1,
            1, 0, 1, 1, 0, 1,
        };
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = sizeof(quad);
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device_, &bi, nullptr, &vertex_buf_);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, vertex_buf_, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &ai, nullptr, &vertex_mem_);
        vkBindBufferMemory(device_, vertex_buf_, vertex_mem_, 0);
        void *p = nullptr;
        vkMapMemory(device_, vertex_mem_, 0, VK_WHOLE_SIZE, 0, &p);
        memcpy(p, quad, sizeof(quad));
        vkUnmapMemory(device_, vertex_mem_);

        instance_capacity_ = 16384;
        bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = (VkDeviceSize)instance_capacity_ * sizeof(InstanceData);
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult br = vkCreateBuffer(device_, &bi, nullptr, &instance_buf_);
        vkGetBufferMemoryRequirements(device_, instance_buf_, &req);
        ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        u32 mt = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ai.memoryTypeIndex = mt;
        VkResult ar = vkAllocateMemory(device_, &ai, nullptr, &instance_mem_);
        VkResult rr = vkBindBufferMemory(device_, instance_buf_, instance_mem_, 0);
        VkResult mr = vkMapMemory(device_, instance_mem_, 0, VK_WHOLE_SIZE, 0, &instance_mapped_);
        if (br != VK_SUCCESS || ar != VK_SUCCESS || rr != VK_SUCCESS || mr != VK_SUCCESS)
        {
            std::fprintf(stderr,
                         "[Fox] instance buffer failed (create=%d alloc=%d bind=%d map=%d)\n",
                         (int)br, (int)ar, (int)rr, (int)mr);
            return false;
        }
        return true;
    }

    bool VkBackend::create_sampler()
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        // mipmapMode=LINEAR：三线性过滤的**跨级**那一半。没有它，上层
        // mip 即便上传了也永远不会被选中（LOD 量化到最近一层后再取 level 0
        // 的最近邻），强缩小下的走样与无 mip 完全一致。
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        // 上限给足：链最长 ~log2(16384)=14 级。无 mip 的纹理（图集等）视图
        // levelCount=1，LOD 恒为 0，这个上限对它们没有影响。
        ci.maxLod = 16.0f;
        // 曾经这里是个 DOCUMENTED GAP：sampler 只采 level 0、上传也只传
        // 一级，PNG 自带的 mip 链（image/png.cpp）在 GPU 上被整个丢弃 ——
        // 强缩小下 GPU 比 CPU  rasteriser 走样更重。现已补齐：见下方
        // mipmapMode/maxLod 与 upload_texture 的逐级拷贝。
        return vkCreateSampler(device_, &ci, nullptr, &sampler_) == VK_SUCCESS;
    }

    // ── 管线 ───────────────────────────────────────────────────────

    bool VkBackend::create_pipeline()
    {
        VkShaderModuleCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        // 内嵌的 SPIR-V 是字节数组；VkShaderModule 要的是 uint32_t* 字。
        // uint8_t[] 的对齐是 1，reinterpret_cast 不严格保证 4 字节对齐——
        // 但数组是文件作用域的，编译器至少按元素需要对齐，实际通常对齐到 16。
        // 为稳妥起见，嵌入脚本在头文件里输出 uint32_t；见 shaders_spv.h。
        vci.codeSize = FOX_SPV_VERT_SIZE;
        vci.pCode = fox_spv_vert;
        VkShaderModule vs = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &vci, nullptr, &vs) != VK_SUCCESS) return false;
        vci.codeSize = FOX_SPV_FRAG_SIZE;
        vci.pCode = fox_spv_frag;
        VkShaderModule fs = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &vci, nullptr, &fs) != VK_SUCCESS) return false;

        // 描述符布局：帧 UBO + 字体图集 + 模糊背景 + 图像。
        //
        // 四个单条目 binding，而不是 sampler 数组：Vulkan 要求聚合成数组的
        // sampler 必须用常量整型表达式索引
        // （VUID-VkDescriptorSetLayoutBinding-descriptorType-06482），按实例
        // 索引纹理这条路走不通。Fox 改为更换描述符 SET，代价是接缝处多一次
        // 重新绑定。
        VkDescriptorSetLayoutBinding binds[4]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[2].binding = 2;
        binds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[2].descriptorCount = 1;
        binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[3].binding = 3;
        binds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[3].descriptorCount = 1;
        binds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo slci{};
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = 4;
        slci.pBindings = binds;
        vkCreateDescriptorSetLayout(device_, &slci, nullptr, &set_layout_);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout_;
        vkCreatePipelineLayout(device_, &plci, nullptr, &pipe_layout_);

        VkVertexInputBindingDescription vbinds[2] = {};
        vbinds[0].binding = 0;
        vbinds[0].stride = 8; // vec2 单位四边形
        vbinds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vbinds[1].binding = 1;
        vbinds[1].stride = sizeof(InstanceData);
        vbinds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        VkVertexInputAttributeDescription attrs[7]{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attrs[1] = {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, rect)};
        attrs[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, radii)};
        attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, color)};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, color2)};
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, params)};
        attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, params2)};

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = vbinds;
        vi.vertexAttributeDescriptionCount = 7;
        vi.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // Viewport + scissor 每次绘制时设置（vkCmdSetViewport/SetScissor），
        // 因为窗口可能在帧间改变尺寸，把 extent 烧进管线就要求重建管线。
        // 两种动态状态都必须声明，否则 pViewports/pScissors 必须非空——两个
        // 都不设会让驱动在 vkCreateGraphicsPipelines 里崩溃。
        VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn;
        gp.layout = pipe_layout_;
        gp.subpass = 0;

        // render pass 按 swapchain 格式延迟创建，所以在这里构建。
        VkAttachmentDescription att{};
        att.format = color_format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Swapchain pass。initialLayout 是 UNDEFINED，不是 PRESENT_SRC_KHR。
        //
        // loadOp 是 CLEAR，上一帧的内容反正会被丢弃，声明 UNDEFINED 是告诉
        // 驱动"我不在乎它处于什么状态"——驱动就可以省掉一次真正的 barrier。
        // finalLayout 仍必须是 PRESENT_SRC_KHR，因为 vkQueuePresentKHR 要求
        // 这个 layout 才能把图像收回去呈现。
        //
        // 在这里写 PRESENT_SRC_KHR 会让校验层把图像当作首次使用时是 UNDEFINED，
        // 而 pass 断言不是，它会把这个不一致报到绘制时绑定的那个描述符上
        // （VUID-vkCmdDraw-None-09600）——指向 u_blurred 和 swapchain 图像，
        // 而这两者其实都没参与。
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;

        VkRenderPassCreateInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        vkCreateRenderPass(device_, &rp, nullptr, &render_pass_);

        // 离屏变体：进 UNDEFINED，出 COLOR_ATTACHMENT_OPTIMAL，让模糊 pass
        // 每帧都有明确的起始 layout。
        VkAttachmentDescription satt = att;
        satt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        satt.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkRenderPassCreateInfo srp = rp;
        srp.pAttachments = &satt;
        vkCreateRenderPass(device_, &srp, nullptr, &scene_pass_);

        gp.renderPass = render_pass_;
        if (!render_pass_ || !scene_pass_)
        {
            std::fprintf(stderr, "[Fox] vkCreateRenderPass failed\n");
            return false;
        }
        VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe_);
        if (r != VK_SUCCESS)
        {
            std::fprintf(stderr, "[Fox] vkCreateGraphicsPipelines = %d\n", (int)r);
            vkDestroyShaderModule(device_, vs, nullptr);
            vkDestroyShaderModule(device_, fs, nullptr);
            return false;
        }

        // 同样的管线描述，针对离屏 render pass。必须在 shader module 被销毁
        // 之前做——两条管线都持有对它们的引用
        // （VUID-VkPipelineShaderStageCreateInfo-module-parameter）。
        VkGraphicsPipelineCreateInfo gp_scene = gp;
        gp_scene.renderPass = scene_pass_;
        VkResult r2 =
            vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp_scene, nullptr, &scene_pipe_);

        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);

        if (r2 != VK_SUCCESS)
        {
            std::fprintf(stderr, "[Fox] offscreen pipeline creation failed (%d)\n", (int)r2);
            return false;
        }
        ensure_framebuffers();
        return true;
    }

    // 帧缓冲依赖 render pass，所以在 create_pipeline() 之后、以及每次
    // swapchain 重建之后（重）建。
    void VkBackend::ensure_framebuffers()
    {
        for (auto f : swap_fb_)
            if (f) vkDestroyFramebuffer(device_, f, nullptr);
        swap_fb_.clear();
        if (!render_pass_ || swap_views_.empty()) return;

        swap_fb_.resize(swap_views_.size());
        for (size_t i = 0; i < swap_views_.size(); ++i)
        {
            VkFramebufferCreateInfo fbi{};
            fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbi.renderPass = render_pass_;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &swap_views_[i];
            fbi.width = extent_.width;
            fbi.height = extent_.height;
            fbi.layers = 1;
            vkCreateFramebuffer(device_, &fbi, nullptr, &swap_fb_[i]);
        }
    }

    // ── 字体槽 + 描述符 ───────────────────────────────────────────────

    FontId VkBackend::slot_of(FontId f) const
    {
        for (size_t i = 0; i < font_slots_.size(); ++i)
            if (font_slots_[i].id == f) return (FontId)(i + 1); // 槽位从 1 开始
        return FONT_INVALID;
    }

    VkBackend::FontSlot *VkBackend::slot(FontId f)
    {
        FontId s = slot_of(f);
        if (s == FONT_INVALID) return nullptr;
        return &font_slots_[s - 1];
    }

    int VkBackend::ensure_slot(FontId f)
    {
        FontId s = slot_of(f);
        if (s != FONT_INVALID) return (int)s - 1;
        // 槽位只在图集上传时创建。提前创建会让批次循环拿到一个图像还不存在
        // 的集合下标，绑定那样的集合是 VUID-vkCmdDraw-None-09600。
        if (!desc_pool_) return -1;
        font_slots_.push_back(FontSlot{});
        font_slots_.back().id = f;
        // ⚠ 不是 desc_sets_.push_back()。新的字体集合必须落在下标 1 + k，
        //   k 是它的槽位下标——追加会把它放到数组末尾，只有在完全没有纹理
        //   集合时才碰巧正确。write_descriptors() 和绘制循环都按这个固定顺序
        //   读数组，追加会悄悄改掉角色：一个写着字形图集的集合会被当纹理画。
        //   正是这种错位造成了销毁时的堆损坏。重建只花一次池重建，应用生命
        //   周期里也就发生几次。
        if (!rebuild_desc_sets())
        {
            font_slots_.pop_back();
            return -1;
        }
        return (int)font_slots_.size() - 1;
    }

    // 按固定顺序重新分配整个描述符集数组。
    //
    // 刻意做全量重建而不是巧妙插入：数组的顺序是三处共享的约定，每条"聪明"
    // 的更新路径都是又一种搞错它的方式。池重建很便宜，而且不会失同步。
    bool VkBackend::rebuild_desc_sets()
    {
        if (!set_layout_) return false;
        const size_t want = 1 + font_slots_.size() + tex_slots_.size();
        const u32 sets = (u32)want;

        // 池容量按 `want` 个集合分配，并给再冒出几个槽位留余量。
        // 每个集合用 1 个 UBO + 3 个组合图像采样器。
        VkDescriptorPoolSize ps[2]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps[0].descriptorCount = sets + 8;
        ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps[1].descriptorCount = sets * 3 + 24;

        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = sets + 8;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS)
        {
            desc_pool_ = VK_NULL_HANDLE;
            return false;
        }

        desc_sets_.assign(want, VK_NULL_HANDLE);
        for (size_t i = 0; i < want; ++i)
        {
            VkDescriptorSetAllocateInfo dai{};
            dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dai.descriptorPool = desc_pool_;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &set_layout_;
            if (vkAllocateDescriptorSets(device_, &dai, &desc_sets_[i]) != VK_SUCCESS ||
                !desc_sets_[i])
            {
                std::fprintf(stderr, "[Fox] vkAllocateDescriptorSets failed at %zu of %zu\n", i,
                             want);
                desc_sets_[i] = VK_NULL_HANDLE;
            }
        }
        return true;
    }

    // 给一个槽位写 binding 1（字形图集），给所有集合写 binding 0/2。
    // 可以反复调用；只碰变了的部分。
    void VkBackend::write_descriptors()
    {
        VkDescriptorBufferInfo dbi{ubo_, 0, 16};

        // Vulkan 既要求不能往 COMBINED_IMAGE_SAMPLER 写空 imageView
        // （VUID-...-descriptorType-02997），又要求绘制时不能有 binding 未写入
        // （VUID-vkCmdDraw-None-08114）。在任何图集或模糊目标存在之前根本没有
        // 图像可填，所以用 1x1 占位图顶上。它在这里惰性创建，因为它的 layout
        // 转换需要命令池。
        // 还没有可绑定的东西：sampler 在描述符池之后才创建，而第一次写入可能
        // 发生在两者都存在之前。用空 sampler 写描述符会得到绑着垃圾的集合。
        if (!sampler_) return;
        if (!dummy_view_ && !create_dummy_image()) return;

        VkImageView safe_blur = blur_view_ ? blur_view_ : dummy_view_;
        if (!safe_blur) return;

        // ⚠ 下标映射：desc_sets_[0] 是纯图形集合，desc_sets_[1 + k] 属于
        //   font_slots_[k]，desc_sets_[1 + nFonts + m] 属于 tex_slots_[m]。
        //   绘制循环把解析出的下标存在每个 Batch 上，三者都不必在绘制时重新
        //   推导。用同一个下标推导 `fs` 能让两个 vector 保持同步；拿任意
        //   desc_sets_[i] 去读 font_slots_[i-1] 正是当年让后面的字体采样到
        //   swapchain 图像的原因。
        const size_t font_base = 1;
        const size_t tex_base = 1 + font_slots_.size();

        for (size_t i = 0; i < desc_sets_.size(); ++i)
        {
            VkImageView fv = dummy_view_;
            if (i >= font_base && (i - font_base) < font_slots_.size())
            {
                const FontSlot &fs = font_slots_[i - font_base];
                if (fs.view && fs.uploaded) fv = fs.view;
            }

            // Binding 3（u_tex）：本集合的图像纹理。属于字体槽的集合绑占位图，
            // 因为着色器只对 PTYPE_IMAGE 采样 u_tex。
            VkImageView tv = dummy_view_;
            if (i >= tex_base && (i - tex_base) < tex_slots_.size())
            {
                const TexSlot &ts = tex_slots_[i - tex_base];
                if (ts.view && ts.uploaded) tv = ts.view;
            }

            // Binding 2（u_blurred）：只在模糊图像当前处于
            // SHADER_READ_ONLY_OPTIMAL 时才绑真正的它。record_blur_pass 在每帧
            // 末尾把它放到那个状态，到下一帧的 record_blur_pass 之间都保持，
            // 所以下面的检查是准确的。当它正处于转换中（从 upload_atlas 调进来，
            // 那发生在模糊 pass 之前）时，改绑占位图。
            VkImageView bv = dummy_view_;
            if (blur_view_ && blur_layout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                bv = blur_view_;

            VkDescriptorImageInfo dii_font{sampler_, fv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo dii_blur{sampler_, bv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo dii_tex{sampler_, tv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

            VkWriteDescriptorSet w[4]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = desc_sets_[i];
            w[0].dstBinding = 0;
            w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[0].pBufferInfo = &dbi;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = desc_sets_[i];
            w[1].dstBinding = 1;
            w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[1].pImageInfo = &dii_font;
            w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[2].dstSet = desc_sets_[i];
            w[2].dstBinding = 2;
            w[2].descriptorCount = 1;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[2].pImageInfo = &dii_blur;
            w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[3].dstSet = desc_sets_[i];
            w[3].dstBinding = 3;
            w[3].descriptorCount = 1;
            w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[3].pImageInfo = &dii_tex;
            vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
        }
        tex_desc_generation_ = ctx_ ? ctx_->texture_generation() : 0;
    }

    bool VkBackend::create_dummy_image()
    {
        if (dummy_view_) return true;
        if (!cmd_pool_) return false; // layout 转换需要一个命令缓冲
        if (!make_image(1, 1, VK_FORMAT_R8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, dummy_img_,
                        dummy_mem_, dummy_view_))
            return false;

        // 清零一个纹素让图像完全定义，然后留在 SHADER_READ_ONLY_OPTIMAL，
        // 以便立即可绑定。
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cai, &cmd);
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);

        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = 1;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer staging = VK_NULL_HANDLE;
        vkCreateBuffer(device_, &bi, nullptr, &staging);
        VkMemoryRequirements rq{};
        vkGetBufferMemoryRequirements(device_, staging, &rq);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = rq.size;
        ai.memoryTypeIndex =
            find_mem_type(rq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory sm = VK_NULL_HANDLE;
        vkAllocateMemory(device_, &ai, nullptr, &sm);
        vkBindBufferMemory(device_, staging, sm, 0);
        void *sp = nullptr;
        vkMapMemory(device_, sm, 0, VK_WHOLE_SIZE, 0, &sp);
        if (sp) *(u8 *)sp = 0;
        vkUnmapMemory(device_, sm);

        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // 1x1 占位纹理：单层，无 mip（本函数内的 levels 仅为 Barrier 复用）。
        const u32 levels = 1;
        bar.image = dummy_img_;
        bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bar.subresourceRange.levelCount = levels;
        bar.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, staging, dummy_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);

        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &bar);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo sub{};
        sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cmd;
        vkQueueSubmit(queue_, 1, &sub, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, sm, nullptr);
        return true;
    }

    bool VkBackend::create_descriptors()
    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = 16; // vec2 resolution + float time + pad（16 字节 UBO）
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device_, &bi, nullptr, &ubo_);

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, ubo_, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &ai, nullptr, &ubo_mem_);
        vkBindBufferMemory(device_, ubo_, ubo_mem_, 0);
        vkMapMemory(device_, ubo_mem_, 0, VK_WHOLE_SIZE, 0, &ubo_mapped_);

        // 描述符池和集合数组由 rebuild_desc_sets() 构建，它也按当前槽位数
        // 决定大小。

        // 槽位 0 留给"纯图形"批次，永远存在。固定数组是
        // 1 + 字体槽 + 纹理槽；任一数量增长时重建（见 rebuild_desc_sets）。
        if (!rebuild_desc_sets()) return false;
        return true;
    }

    // ── 应用纹理 ─────────────────────────────────────────────────────

    int VkBackend::ensure_tex_slot(TexId t)
    {
        if (t == TEX_NONE) return -1;
        for (size_t i = 0; i < tex_slots_.size(); ++i)
            if (tex_slots_[i].id == t) return (int)i;
        tex_slots_.push_back(TexSlot{});
        tex_slots_.back().id = t;
        return (int)tex_slots_.size() - 1;
    }

    int VkBackend::tex_slot_index(TexId t) const
    {
        if (t == TEX_NONE) return -1;
        for (size_t i = 0; i < tex_slots_.size(); ++i)
            if (tex_slots_[i].id == t) return (int)i;
        return -1;
    }

    bool VkBackend::upload_texture(const Context::Texture &src, TexSlot &ts)
    {
        // 层数来自 Context 侧的 mip 链（image/png.cpp 生成）；无链的纹理
        // （字形图集等）就是 1 级，行为与从前逐字节一致。
        const u32 levels = src.mip_levels();

        // 尺寸**或层数**变化都要重建图像：层数是 vkCreateImage 的参数，
        // 已创建的图像不能追加层级；拿 1 级图像去拷 5 级数据，
        // vkCmdCopyBufferToImage 只会静默忽略越界区域（subresourceRange
        // 之外不报错），结果是「传了却没上传」。
        if (ts.img && (ts.w != src.w || ts.h != src.h || ts.mip_levels != src.mip_levels()))
        {
            vkDestroyImageView(device_, ts.view, nullptr);
            vkDestroyImage(device_, ts.img, nullptr);
            vkFreeMemory(device_, ts.mem, nullptr);
            ts.img = VK_NULL_HANDLE;
            ts.view = VK_NULL_HANDLE;
            ts.uploaded = false;
        }

        if (!ts.img)
        {
            // RGBA8 直通 alpha：预乘由混合状态做，所以纹理存的就是调用方
            // 上传的原样数据。
            if (!make_image(src.w, src.h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ts.img,
                            ts.mem, ts.view, levels))
            {
                // 槽位保持未上传：build_instances() 会跳过它，一次上传失败
                // 顶多缺图标一帧，而不是让描述符引用一个不存在的图像。
                return false;
            }
            ts.w = src.w;
            ts.h = src.h;
            ts.mip_levels = levels;
        }
        if (!ts.img || !ts.view) return false;

        // ── 逐级收集源数据：全部层级进同一个 staging 缓冲，一次拷贝完成 ──
        // 第 0 级用 src.rgba（与 mips.levels[0] 内容相同，但无链纹理只有它）。
        std::vector<const u8 *> level_ptr(levels, nullptr);
        std::vector<VkDeviceSize> level_bytes(levels, 0);
        VkDeviceSize total = 0;
        for (u32 lv = 0; lv < levels; ++lv)
        {
            const u8 *px = nullptr;
            VkDeviceSize n = 0;
            if (lv == 0)
            {
                px = src.rgba.data();
                n = (VkDeviceSize)src.w * src.h * 4;
            }
            else if (lv < src.mips.levels.size())
            {
                px = src.mips.levels[lv].data();
                n = (VkDeviceSize)src.mips.levels[lv].size();
            }
            if (!px || n == 0)
            {
                // 链不完整（理论上 build_mips 保证完整）→ 放弃整个上传，
                // 让下一帧重试。上传一半的链比没有链更难排查。
                std::fprintf(stderr, "[Fox] texture %ux%u mip chain incomplete at level %u\n",
                             src.w, src.h, lv);
                return false;
            }
            level_ptr[lv] = px;
            level_bytes[lv] = n;
            total += n;
        }
        const VkDeviceSize bytes = total;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer staging = VK_NULL_HANDLE;
        vkCreateBuffer(device_, &bi, nullptr, &staging);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, staging, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory smem = VK_NULL_HANDLE;
        vkAllocateMemory(device_, &ai, nullptr, &smem);
        vkBindBufferMemory(device_, staging, smem, 0);
        void *p = nullptr;
        vkMapMemory(device_, smem, 0, VK_WHOLE_SIZE, 0, &p);
        if (p)
        {
            u8 *dst = (u8 *)p;
            for (u32 lv = 0; lv < levels; ++lv)
            {
                std::memcpy(dst, level_ptr[lv], (size_t)level_bytes[lv]);
                dst += level_bytes[lv];
            }
        }
        vkUnmapMemory(device_, smem);

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cai, &cmd);
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);

        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.oldLayout = ts.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.image = ts.img;
        bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bar.subresourceRange.levelCount = levels;
        bar.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        // 逐级拷贝：每级自己的 mipLevel / bufferOffset / extent。
        // extent 逐级减半、最小 1x1 —— 与 image/png.cpp 的 build_mips 一致。
        std::vector<VkBufferImageCopy> copies(levels);
        VkDeviceSize off = 0;
        for (u32 lv = 0; lv < levels; ++lv)
        {
            VkBufferImageCopy &c = copies[lv];
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.mipLevel = lv;
            c.imageSubresource.layerCount = 1;
            c.bufferOffset = off;
            u32 lw = src.w >> lv, lh = src.h >> lv;
            if (lw == 0) lw = 1;
            if (lh == 0) lh = 1;
            c.imageExtent = {lw, lh, 1};
            off += level_bytes[lv];
        }
        vkCmdCopyBufferToImage(cmd, staging, ts.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels,
                               copies.data());

        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &bar);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo sub{};
        sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cmd;
        vkQueueSubmit(queue_, 1, &sub, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, smem, nullptr);

        ts.uploaded = true;
        ts.generation = src.generation;
        return true;
    }

    void VkBackend::sync_textures()
    {
        if (!ctx_) return;

        bool need_desc = (ctx_->texture_generation() != tex_desc_generation_);
        bool need_slot = false;

        // 遍历存活的纹理。新增的或代际号变了的纹理分配槽位并上传。
        //
        // Context 侧的池是按 TexId 1:1 索引的 deque，但槽位 vector 是稀疏的
        // （销毁的纹理留下空洞），所以要遍历而不是直接下标访问。
        for (size_t i = 1; i <= ctx_->texture_count(); ++i)
        {
            Context::Texture *t = ctx_->texture_mut((TexId)i);
            if (!t) continue;
            int si = ensure_tex_slot((TexId)i);
            if (si < 0) continue;
            TexSlot &ts = tex_slots_[(size_t)si];
            if (!ts.uploaded || ts.generation != t->generation)
            {
                if (upload_texture(*t, ts)) need_slot = true;
            }
            if (t->dirty) t->dirty = false;
        }

        // 每个纹理槽需要自己的描述符集，在固定位置上。重建而不是追加——
        // 见 rebuild_desc_sets()。
        const size_t want = 1 + font_slots_.size() + tex_slots_.size();
        if (desc_sets_.size() != want)
        {
            if (!rebuild_desc_sets()) return;
            need_desc = true;
        }

        if (need_desc || need_slot) write_descriptors();
    }

    // ── 离屏（glass 背景链）────────────────────────────────────────

    // 成员函数：需要 device_ 和 find_mem_type。
    //
    // ⚠ 任何一步失败都返回 false，并把所有句柄留空。
    //
    //   忽略 vkCreateImage 的结果，就会得到一个非空但无效的 view，之后被写进
    //   描述符集：驱动接受写入，然后采样到垃圾，损坏要到很晚才在
    //   vkDestroyDevice 里以崩溃形式暴露，没有任何校验消息指向有用的地方。
    //   createImage 可能暂时性失败（设备丢失、内存压力、驱动拒绝某格式），
    //   所以"昨天还好好的"不是安全假设。
    /// 图像的最大合理 mip 层数：1 + floor(log2(min(w,h)))。
    inline u32 mip_dim_max(u32 w, u32 h)
    {
        u32 m = w < h ? w : h;
        u32 n = 0;
        while (m > 1) { m >>= 1; ++n; }
        return n + 1;
    }

    bool VkBackend::make_image(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImage &img,
                               VkDeviceMemory &mem, VkImageView &view, u32 mip_levels)
    {
        img = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        // Level count 是图像（分配 + 视图）的属性，创建后无法补救。在此钳制
        // 而不是信任调用方：链长于图像层数时，上层会被 vkCmdCopyBufferToImage 的
        // subresourceRange 静默排除（不报错，只是永不被读 → 采样到未定义数据）。
        if (mip_levels < 1) mip_levels = 1;
        if (mip_levels > mip_dim_max(w, h)) mip_levels = mip_dim_max(w, h);
        ci.mipLevels = mip_levels;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ci, nullptr, &img) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[Fox] vkCreateImage %ux%u fmt=%d failed\n", w, h, (int)fmt);
            img = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, img, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS ||
            vkBindImageMemory(device_, img, mem, 0) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[Fox] image %ux%u memory setup failed\n", w, h);
            vkDestroyImage(device_, img, nullptr);
            if (mem) vkFreeMemory(device_, mem, nullptr);
            img = VK_NULL_HANDLE;
            mem = VK_NULL_HANDLE;
            return false;
        }

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        // 视图必须覆盖**全部** mip 层：sampler 的 mipmapMode 只在视图的
        // levelCount 范围内选择 LOD，少一层就是高层采样永远走不到。
        vi.subresourceRange.levelCount = mip_levels;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &vi, nullptr, &view) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[Fox] vkCreateImageView %ux%u failed\n", w, h);
            vkDestroyImage(device_, img, nullptr);
            vkFreeMemory(device_, mem, nullptr);
            img = VK_NULL_HANDLE;
            mem = VK_NULL_HANDLE;
            view = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool VkBackend::ensure_offscreen()
    {
        if (scene_img_) return true;
        u32 w = extent_.width, h = extent_.height;
        if (!w || !h) return false;
        // scene：整个 UI 先画到这里，再降采样。TRANSFER_SRC 是必需的——
        // 没有它，vkCmdBlitImage 会从一个不是为传输创建的图像读取，行为未定义，
        // 且静默得到一片黑背景。
        if (!make_image(w, h, color_format_,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        scene_img_, scene_mem_, scene_view_))
            return false;
        scene_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        // blur 目标：由 blit 写入，然后被 glass 着色器采样。除数是质量旋钮
        // （见 core/context.hpp）：降采样本身就是模糊，所以减半分辨率就是
        // 最便宜又诚实的降本方式。
        blur_w_ = fx_max(1u, w / blur_div_);
        blur_h_ = fx_max(1u, h / blur_div_);
        if (!make_image(blur_w_, blur_h_, color_format_,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, blur_img_,
                        blur_mem_, blur_view_))
        {
            destroy_offscreen();
            return false;
        }
        blur_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        auto make_fb = [&](VkImageView v, VkRenderPass rp, VkFramebuffer &out, u32 fw, u32 fh) {
            VkFramebufferCreateInfo fbi{};
            fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbi.renderPass = rp;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &v;
            fbi.width = fw;
            fbi.height = fh;
            fbi.layers = 1;
            vkCreateFramebuffer(device_, &fbi, nullptr, &out);
        };
        make_fb(scene_view_, scene_pass_, scene_fb_, w, h);
        // blur_fb_ 故意不创建：blur 目标只由 vkCmdBlitImage 写入，从不经过
        // render pass，所以不需要帧缓冲。新创建的图像处于 UNDEFINED；
        // record_blur_pass 第一帧必须从那里做转换，而不是从 SHADER_READ_ONLY。
        blur_img_created_ = false;
        write_descriptors();
        return true;
    }

    void VkBackend::destroy_offscreen()
    {
        if (scene_fb_) { vkDestroyFramebuffer(device_, scene_fb_, nullptr); scene_fb_ = VK_NULL_HANDLE; }
        if (blur_fb_) { vkDestroyFramebuffer(device_, blur_fb_, nullptr); blur_fb_ = VK_NULL_HANDLE; }
        if (scene_view_) { vkDestroyImageView(device_, scene_view_, nullptr); scene_view_ = VK_NULL_HANDLE; }
        if (scene_img_) { vkDestroyImage(device_, scene_img_, nullptr); scene_img_ = VK_NULL_HANDLE; }
        if (scene_mem_) { vkFreeMemory(device_, scene_mem_, nullptr); scene_mem_ = VK_NULL_HANDLE; }
        if (blur_view_) { vkDestroyImageView(device_, blur_view_, nullptr); blur_view_ = VK_NULL_HANDLE; }
        if (blur_img_) { vkDestroyImage(device_, blur_img_, nullptr); blur_img_ = VK_NULL_HANDLE; }
        if (blur_mem_) { vkFreeMemory(device_, blur_mem_, nullptr); blur_mem_ = VK_NULL_HANDLE; }
        // resize 后 blur 目标是新图像，layout 回到 UNDEFINED。
        // record_blur_pass 用它来选正确的 oldLayout。
        blur_img_created_ = false;
        blur_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        scene_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        write_descriptors(); // 上传前把 binding 2 重新绑回占位图
    }

    // ── 字形图集上传 ─────────────────────────────────────────────────────

    bool VkBackend::upload_atlas(FontId id)
    {
        GlyphAtlas *atlas = fonts().atlas(id);
        if (!atlas) return false;
        int w = atlas->width(), h = atlas->height();
        if (w <= 0 || h <= 0) return false;
        const std::vector<u8> &px = atlas->pixels();

        // 图像必须在写槽位的描述符集之前存在，否则集合会绑到空 imageView
        // （VUID-...-descriptorType-02997）。
        int si = ensure_slot(id);
        if (si < 0) return false;
        FontSlot &sl = font_slots_[(size_t)si];

        // 只在没有合适尺寸的图像时才（重）建 GPU 图像。
        if (!sl.img)
        {
            make_image((u32)w, (u32)h, VK_FORMAT_R8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sl.img,
                       sl.mem, sl.view);
            // 新图像：layout 是 UNDEFINED。这个标志让下面的 barrier 从那里
            // 转换，而不是从 SHADER_READ_ONLY
            // （VUID-VkImageMemoryBarrier-oldLayout-01197）。
            sl.uploaded = false;
        }

        // 暂存 -> 拷贝。只在图集确实新增了字形时才跑，这正是 dirty 标志的
        // 全部意义。
        VkDeviceSize bytes = (VkDeviceSize)px.size();
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer staging = VK_NULL_HANDLE;
        vkCreateBuffer(device_, &bi, nullptr, &staging);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, staging, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;
        vkAllocateMemory(device_, &ai, nullptr, &staging_mem);
        vkBindBufferMemory(device_, staging, staging_mem, 0);
        void *p = nullptr;
        vkMapMemory(device_, staging_mem, 0, VK_WHOLE_SIZE, 0, &p);
        memcpy(p, px.data(), px.size());
        vkUnmapMemory(device_, staging_mem);

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cai, &cmd);
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);

        // 图像可能因上次上传而已处于 SHADER_READ_ONLY；硬做
        // UNDEFINED->TRANSFER_DST 转换会把内容丢掉，更糟的是让旧 layout 变成
        // 对什么都没有的隐式依赖。
        // 字形图集按 1:1 采样、每次新增字形整幅重传 —— 不给它建 mip 链
        // （见 Context::Texture::mips 的说明），故层数恒为 1。
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        const u32 levels = 1;
        bar.oldLayout = sl.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.image = sl.img;
        bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bar.subresourceRange.levelCount = levels;
        bar.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {(u32)w, (u32)h, 1};
        vkCmdCopyBufferToImage(cmd, staging, sl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &bar);

        vkEndCommandBuffer(cmd);
        // 叫 `sub` 是因为本函数已经用 `si` 表示字体槽下标；再遮蔽它会把
        // 提交信息变成一个 int。
        VkSubmitInfo sub{};
        sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cmd;
        vkQueueSubmit(queue_, 1, &sub, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);

        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);

        sl.uploaded_glyphs = atlas->glyph_count();
        sl.uploaded = true;
        atlas->clear_dirty();
        write_descriptors();
        return true;
    }


    // ================= 输入 =================

    // 电平状态直接从 GLFW 轮询；边沿通过与上一帧比较得出。GLFW 回调只处理
    // 轮询表达不了的东西：Unicode 码点和滚轮累积。
    struct EdgeTracker
    {
        bool mouse_down[3] = {false, false, false};
        // KEY_SLOTS，不是 256：Key::F12 是 0x11C。见 input.hpp——这里数组
        // 偏短的话，一按具名键就越界写全局缓冲。
        bool key_down[KEY_SLOTS] = {false};
    };
    static EdgeTracker g_edges;

    // glfwGetKeyMods 在 GLFW 3.4 中被移除；直接查修饰键。
    static int fx_mods(GLFWwindow *w)
    {
        int m = 0;
        if (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
            m |= GLFW_MOD_SHIFT;
        if (glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
            glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)
            m |= GLFW_MOD_CONTROL;
        if (glfwGetKey(w, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
            glfwGetKey(w, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS)
            m |= GLFW_MOD_ALT;
        if (glfwGetKey(w, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
            glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS)
            m |= GLFW_MOD_SUPER;
        return m;
    }

    void VkBackend::poll_events(Context &ctx)
    {
        glfwPollEvents();
        // 窗口自己的关闭请求进 context，这样 C ABI 的 fox_should_close() 不用
        // 伸手进后端就能报告它（无头 context 也不会被误判为"应关闭"）。
        ctx.set_backend_close_requested(window_close_requested());
        InputState &in = ctx.input();

        int mods = fx_mods(window_);
        in.shift = (mods & GLFW_MOD_SHIFT) != 0;
        in.ctrl = (mods & GLFW_MOD_CONTROL) != 0;
        in.alt = (mods & GLFW_MOD_ALT) != 0;
        in.super_key = (mods & GLFW_MOD_SUPER) != 0;

        double mx = 0, my = 0;
        glfwGetCursorPos(window_, &mx, &my);
        in.mouse_pos = Vec2{(f32)mx, (f32)my};

        in.scroll_x = (f32)g_scroll_x_;
        in.scroll_y = (f32)g_scroll_y_;
        in.mouse_scroll = Vec2{in.scroll_x, in.scroll_y};
        g_scroll_x_ = 0.0;
        g_scroll_y_ = 0.0;

        const int glfw_btn[3] = {GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT,
                                 GLFW_MOUSE_BUTTON_MIDDLE};
        static double last_down_t[3] = {0, 0, 0};
        static int click_n[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i)
        {
            bool now = glfwGetMouseButton(window_, glfw_btn[i]) == GLFW_PRESS;
            bool prev = g_edges.mouse_down[i];
            ButtonState &b = in.buttons[i];
            b.down = now;
            b.pressed = now && !prev;
            b.released = !now && prev;
            if (b.pressed)
            {
                b.x = in.mouse_pos.x;
                b.y = in.mouse_pos.y;
                b.down_time = ctx.time();
                double t = glfwGetTime();
                click_n[i] = (t - last_down_t[i] < 0.4) ? click_n[i] + 1 : 1;
                last_down_t[i] = t;
                b.click_count = click_n[i];
                b.double_click = (click_n[i] >= 2);
            }
            if (b.released) b.up_time = ctx.time();
            g_edges.mouse_down[i] = now;
        }

        // 可打印 ASCII：GLFW_KEY_A..GLFW_KEY_Z 映射到 'a'..'z'。
        // 数字/标点由 Unicode 字符回调覆盖。
        for (int k = 'a'; k <= 'z'; ++k)
        {
            if (k < 0 || k >= KEY_SLOTS) continue;
            int gk = GLFW_KEY_A + (k - 'a');
            bool now = glfwGetKey(window_, gk) == GLFW_PRESS;
            bool prev = g_edges.key_down[k];
            in.keys_down[k] = now;
            if (now && !prev) in.keys_pressed[k] = true;
            if (!now && prev) in.keys_released[k] = true;
            g_edges.key_down[k] = now;
        }

        // 具名键在 0x100+，不在旧注释所说的低 ASCII 区——这正是当年数组短了
        // 一个字节、每个具名键都越界写的原因。
        //
        // 每个条目都重新检查边界：这张表是数据，打错一个字否则就是静默越界
        // 写，而不是断言失败。
        struct Named { int glfw; int slot; };
        static const Named named[] = {
            {GLFW_KEY_BACKSPACE, (int)Key::Backspace}, {GLFW_KEY_TAB, (int)Key::Tab},
            {GLFW_KEY_ENTER, (int)Key::Enter},         {GLFW_KEY_ESCAPE, (int)Key::Escape},
            {GLFW_KEY_DELETE, (int)Key::Delete},       {GLFW_KEY_UP, (int)Key::Up},
            {GLFW_KEY_DOWN, (int)Key::Down},           {GLFW_KEY_LEFT, (int)Key::Left},
            {GLFW_KEY_RIGHT, (int)Key::Right},         {GLFW_KEY_SPACE, (int)' '},
        };
        for (const Named &nk : named)
        {
            if (nk.slot < 0 || nk.slot >= KEY_SLOTS) continue;
            bool now = glfwGetKey(window_, nk.glfw) == GLFW_PRESS;
            bool prev = g_edges.key_down[nk.slot];
            in.keys_down[nk.slot] = now;
            if (now && !prev) in.keys_pressed[nk.slot] = true;
            if (!now && prev) in.keys_released[nk.slot] = true;
            g_edges.key_down[nk.slot] = now;
        }

        // Unicode 码点（覆盖数字、标点和 CJK IME 输入）。
        in.chars = g_typed_chars;
        g_typed_chars.clear();

        // 缩放每帧重读，不只在 resize 时：把窗口拖到密度不同的显示器上，
        // 内容缩放会变，而 framebuffer 尺寸完全不变——只查尺寸就会漏掉，
        // UI 会一直停在旧密度，直到下一次 resize。
        const float sc = content_scale();
        if (ctx.set_scale(sc)) vkDeviceWaitIdle(device_);

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window_, &fbw, &fbh);
        if (getenv("FOX_TRACE"))
        {
            int ww = 0, wh = 0;
            glfwGetWindowSize(window_, &ww, &wh);
            std::fprintf(stderr,
                         "[Fox] sizes: window=%dx%d framebuffer=%dx%d extent=%ux%u scale=%.2f\n",
                         ww, wh, fbw, fbh, extent_.width, extent_.height, content_scale());
        }

        // surface 自己的 currentExtent 才是 swapchain 的**权威**尺寸。
        //
        // ⚠ 窗口刚创建时 surface 往往还没被合成器配置完，currentExtent 会是
        //   一个过时的值（本项目实测 790×477，而窗口已经是 941×1030）。
        //   只按 framebuffer 尺寸判断的话，**永远发现不了「swapchain 比窗口
        //   小」** —— 于是画面只占窗口左上角，其余一片空白。看起来像「UI 没
        //   渲染」，其实是渲染到了一块比窗口小的 surface 上。
        bool extent_stale = false;
        if (surface_ != VK_NULL_HANDLE && phys_ != VK_NULL_HANDLE)
        {
            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) == VK_SUCCESS &&
                caps.currentExtent.width != UINT32_MAX && caps.currentExtent.height != UINT32_MAX &&
                (caps.currentExtent.width != extent_.width || caps.currentExtent.height != extent_.height))
            {
                extent_stale = true;
            }
        }

        if (fbw > 0 && fbh > 0 && (extent_stale || fbw != (int)extent_.width || fbh != (int)extent_.height))
        {
            vkDeviceWaitIdle(device_);
            destroy_swapchain();
            if (!create_swapchain()) fault("swapchain recreation failed (resize)");
            ensure_framebuffers();
            destroy_offscreen();
            ctx.set_framebuffer_size(fbw, fbh);
        }
    }

    void VkBackend::fault(const char *why)
    {
        ++frame_faults_;
        last_fault_ = why ? why : "unspecified fault";
        std::fprintf(stderr, "[Fox] frame fault #%u: %s\n", frame_faults_, last_fault_);
    }

    // ================= 渲染 =================

    void VkBackend::record_blur_pass(VkCommandBuffer cmd)
    {
        // 4 倍 box 降采样。降采样本身就是模糊：一次 blit（线性过滤），
        // 不乒乓、不卷核。开销够小，每帧都能跑——真正含玻璃的帧本来就少。
        auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                           VkAccessFlags dst, VkPipelineStageFlags spipe, VkPipelineStageFlags sdpipe) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = src;
            b.dstAccessMask = dst;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = img;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, spipe, sdpipe, 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        // 离屏 render pass 结束时 scene_img_ 停在 COLOR_ATTACHMENT_OPTIMAL。
        barrier(scene_img_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        scene_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        // blur 图像的 layout 显式跟踪。描述符只有在它处于
        // SHADER_READ_ONLY_OPTIMAL 时引用它才合法，而它每帧会经过
        // UNDEFINED/TRANSFER_DST/TRANSFER_SRC 几个状态。
        // srcAccessMask 必须由 srcStageMask 支持：TRANSFER_READ_BIT 属于
        // TRANSFER 阶段，只有源阶段也是 TRANSFER 时才能出现
        // （VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819）。
        VkAccessFlags src = 0;
        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (blur_layout_ == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            src = 0;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        }
        else
        {
            // 正常稳态：上一帧片段着色器采样过它。
            src = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        barrier(blur_img_, blur_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, src,
                VK_ACCESS_TRANSFER_WRITE_BIT, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {(int32_t)extent_.width, (int32_t)extent_.height, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {(int32_t)blur_w_, (int32_t)blur_h_, 1};
        vkCmdBlitImage(cmd, scene_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, blur_img_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        barrier(blur_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        blur_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        blur_img_created_ = true;
        // ⚠ 不建议在这里调 write_descriptors()。本命令缓冲里已经绑定过的
        //   描述符集会在录制中途失效
        //   （VUID-vkCmdBeginRenderPass-commandBuffer-recording）。延迟标志
        //   由 render() 在帧间处理。
        blur_binding_stale_ = true;
    }

    void VkBackend::render(const DisplayList &dl, Context &ctx)
    {
        // ⚠ 先等上一帧结束，在任何上传或改写描述符之前。
        //
        //   两处独立的竞争在这里汇合：
        //     · 实例缓冲是 host-visible 且常驻映射，GPU 还在读上一帧内容时
        //       覆写它就是数据竞争，表现为间歇、不可复现的几何损坏；
        //     · upload_atlas()/sync_textures() 会调 vkUpdateDescriptorSets，
        //       而改写上一次提交还在引用的描述符同样是竞争——驱动可能采样到
        //       改了一半的集合。
        //
        //   fence 创建时就是 SIGNALED，所以第一次等待立刻返回。
        if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        {
            fault("vkWaitForFences failed (device lost?)");
            return;
        }

        // 必须在构建实例之前：绘制循环要针对 tex_slots_/desc_sets_ 解析每个
        // 批次的描述符集下标，而这些正是这一步创建的。
        sync_textures();

        // 转换本身在 draw/instance.cpp 里，CPU 后端消费的是完全相同的数据。
        // 一份实现，不是两份。
        build_instances(ctx, dl, frame_);
        last_instances_ = (u32)frame_.instances.size();
        last_used_blur_ = frame_.used_glass;
        last_dropped_ = frame_.dropped;
        batches_ = frame_.batches;
        if (getenv("FOX_TRACE"))
            std::fprintf(stderr, "[Fox] build_instances: %u instances, %zu batches, %u dropped\n",
                         last_instances_, batches_.size(), frame_.dropped);

        // 上传本帧新增了字形的每个图集，不只是正文字体。漏了这一步，除了
        // 默认字号外所有文字都是空白：UV 是对的，只是像素不在那里。
        for (const Batch &b : batches_)
        {
            if (b.font == FONT_INVALID) continue;
            GlyphAtlas *a = fonts().atlas(b.font);
            if (!a) continue;
            FontSlot *sl = slot(b.font);
            if (!sl) { if (!upload_atlas(b.font)) continue; continue; }
            if (a->dirty() || sl->uploaded_glyphs != a->glyph_count()) upload_atlas(b.font);
        }

        if (frame_.instances.empty()) return;

        // 用 RESET_COMMAND_BUFFER 开始，这样每帧复用同一个命令缓冲，而不是
        // 分配一个新的、再（错误地）释放一个还在执行的。
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = frame_cmd_;
        if (!cmd)
        {
            vkAllocateCommandBuffers(device_, &cai, &cmd);
            frame_cmd_ = cmd;
        }
        else
        {
            vkResetCommandBuffer(cmd, 0);
        }

        // 实例上传走逻辑单位 → 设备像素（DPI 缩放）。
        //
        // ⚠ 裸 memcpy —— 逻辑单位的矩形被直接当成设备像素用。
        //   1x 显示器上两者相等（看不出问题），2x 上 UI 只占 swapchain 的左上角
        //   1/4，看起来像「界面缩成了一小块」。
        //
        //   CPU 路径**刻意**不缩放（它是逐像素的正确性参照，见
        //   cpu_backend.cpp 的说明）；GPU 路径必须按 DPI 放大，才能填满设备
        //   分辨率的 swapchain。
        const f32 dpi = ctx.scale() > 0.0f ? ctx.scale() : 1.0f;
        if (dpi == 1.0f)
        {
            memcpy(instance_mapped_, frame_.instances.data(),
                   frame_.instances.size() * sizeof(InstanceData));
        }
        else
        {
            InstanceData *dst = (InstanceData *)instance_mapped_;
            for (size_t i = 0; i < frame_.instances.size(); ++i)
            {
                dst[i] = frame_.instances[i];
                scale_instance(dst[i], dpi);
            }
        }

        if (ubo_mapped_)
        {
            float u[4] = {(float)extent_.width, (float)extent_.height, ctx.time(), 0.0f};
            memcpy(ubo_mapped_, u, sizeof(u));
        }

        if (getenv("FOX_TRACE") && instance_mapped_ && !frame_.instances.empty())
        {
            // 上传后的**设备像素**矩形（= 逻辑矩形 × DPI）。看一眼就知道 DPI
            // 换算有没有生效：dpi=2 时这里应当是逻辑值的 2 倍。
            //
            // 打印**最后一个**实例而不是第一个：第一个通常是铺满窗口的背景，
            // 它的设备尺寸两种 dpi 下都一样（永远等于 framebuffer），没有判别力。
            const size_t last = frame_.instances.size() - 1;
            const InstanceData &dl = ((const InstanceData *)instance_mapped_)[last];
            const InstanceData &lg = frame_.instances[last];
            std::fprintf(stderr,
                         "[Fox] last instance: logical %.1f,%.1f %.1fx%.1f  ->  device %.1f,%.1f %.1fx%.1f  (dpi=%.2f)\n",
                         lg.rect[0], lg.rect[1], lg.rect[2], lg.rect[3],
                         dl.rect[0], dl.rect[1], dl.rect[2], dl.rect[3], dpi);
        }

        u32 img = 0;
        VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, sem_available_,
                                            VK_NULL_HANDLE, &img);
        // 描述符更新必须在没有任何命令缓冲绑定这些集合时做，即帧间——绝不能
        // 在 vkBeginCommandBuffer 和 vkEndCommandBuffer 之间。
        if (blur_binding_stale_)
        {
            blur_binding_stale_ = false;
            write_descriptors();
        }
        if (acq == VK_ERROR_OUT_OF_DATE_KHR)
        {
            vkDeviceWaitIdle(device_);
            destroy_swapchain();
            if (!create_swapchain())
            {
                fault("swapchain recreation failed (acquire out-of-date)");
                return;
            }
            ensure_framebuffers();
            destroy_offscreen();
            write_descriptors();
            return;
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        {
            fault("vkAcquireNextImageKHR failed");
            return;
        }

        // 质量档（见 core/context.hpp 的 Quality）：blur 档只改**下采样倍率**，
        // 因为降采样本身就是模糊 —— 不存在第二套模糊实现。档位变化需要重建
        // 离屏图像，所以在这里（没有命令缓冲在录制）比对并重建，而不是等它自己
        // 碰巧失效 —— "改了档位但图像还是旧尺寸"是最难发现的一类错误。
        const u32 want_div = ctx.quality().blur == 1 ? 8u : 4u;
        if (blur_div_ != want_div)
        {
            destroy_offscreen();
            blur_div_ = want_div;
        }

        // blur==2（关）：玻璃不再采样背景，退化成"半透明材质 + 描边"。
        // 语义上仍然是一块玻璃，只是没有背后的模糊 —— 完全丢掉这块面板才是
        // 真正的错误（信息没了，而不只是变丑了）。
        //
        // ⚠ ensure_offscreen() 必须在这里调用，而不是在 vkBeginCommandBuffer
        //   之后：它最后会 write_descriptors()，而描述符写入不能发生在有命令
        //   缓冲正在录制的时候（见本文件顶部关于 blur_binding_stale_ 的说明）。
        //   离屏图像创建的时机因此由帧边界决定，与被谁使用无关。
        const bool want_blur =
            last_used_blur_ && ctx.quality().blur != 2 && !getenv("FOX_NOBLUR");
        const bool use_blur = want_blur && ensure_offscreen();

        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(cmd, &cbi) != VK_SUCCESS)
        {
            fault("vkBeginCommandBuffer failed");
            return;
        }

        VkClearValue clear{};
        const Color &bg = ctx.theme().window_bg;
        clear.color = {{bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1.0f}};

        auto draw_into = [&](VkFramebuffer fb, VkRenderPass rp, VkPipeline pl) {
            VkRenderPassBeginInfo rpb{};
            rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpb.renderPass = rp;
            rpb.framebuffer = fb;
            rpb.renderArea.extent = extent_;
            rpb.clearValueCount = 1;
            rpb.pClearValues = &clear;
            vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);

            VkViewport vp{0, 0, (float)extent_.width, (float)extent_.height, 0, 1};
            VkRect2D sc{{0, 0}, extent_};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buf_, &zero);
            vkCmdBindVertexBuffers(cmd, 1, 1, &instance_buf_, &zero);

            // 每个批次一次绘制，字形图集变化时重新绑定描述符集。批次是有序
            // 连续段，所以绘制顺序精确。-2 是为了让第一个批次必定重新绑定：
            // 没绑描述符集就绘制是未定义行为，而纯图形恰好是第一个批次。
            long current = -2;
            for (const Batch &b : batches_)
            {
                if (b.count == 0) continue;

                // 描述符集在这里解析，不是在构建时。
                //
                // 下标取决于 font_slots_.size() 和 tex_slots_.size()，两者都会在
                // 第一次见到某个字号或纹理时增长——包括在 build_instances 之后
                // 跑的图集上传那一趟。把下标缓存在批次上意味着批次可能绑到一个
                // 角色已悄悄改变的集合，堆损坏就是这么来的。
                //
                // 固定顺序（见 rebuild_desc_sets）：
                //   0                      纯图形——无纹理绑定
                //   1 + k                  字体槽 k
                //   1 + nFonts + m         纹理槽 m
                long ds_index;
                if (b.font != FONT_INVALID)
                {
                    const FontId slot = slot_of(b.font);
                    if (slot == FONT_INVALID) continue;
                    ds_index = (long)slot;
                }
                else if (b.tex != TEX_NONE)
                {
                    const int si = tex_slot_index(b.tex);
                    if (si < 0) continue;
                    ds_index = (long)(1 + font_slots_.size() + si);
                }
                else
                {
                    ds_index = 0;
                }
                if (ds_index < 0 || (size_t)ds_index >= desc_sets_.size()) continue;
                if (!desc_sets_[(size_t)ds_index]) continue;

                if (ds_index != current)
                {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0,
                                            1, &desc_sets_[(size_t)ds_index], 0, nullptr);
                    current = ds_index;
                }
                // 按批次设 scissor。
                //
                // 之前整个渲染只设一次全屏 scissor（render pass 开始处），
                // fox_push_clip/fox_clip_pop 记录的命令被 build_instances 丢弃
                // —— 裁剪从来没有生效过，group 的内容会溢出并盖住边框。
                //
                // 现在按 batch 设：batch 不跨裁剪边界（build_instances 在
                // ClipPush/ClipPop 处切批），所以「一个 batch 一个 scissor」
                // 就是精确的。
                VkRect2D sc{{0, 0}, extent_};
                if (b.clip.w > 0.0f && b.clip.h > 0.0f)
                {
                    // 裁剪矩形与实例一样是**逻辑单位** → 要按 DPI 换算成设备
                    // 像素，否则 2x 上裁剪区会比内容小一半（内容被切掉）。
                    sc.offset.x = (int32_t)(b.clip.x * dpi);
                    sc.offset.y = (int32_t)(b.clip.y * dpi);
                    sc.extent.width = (uint32_t)(b.clip.w * dpi);
                    sc.extent.height = (uint32_t)(b.clip.h * dpi);
                    // Vulkan 要求 scissor 完全落在 framebuffer 内，否则行为未定义
                    // （不是「被裁掉」，是未定义）。所以这里夹一次。
                    if (sc.offset.x < 0)
                    {
                        sc.extent.width = (uint32_t)((int32_t)sc.extent.width + sc.offset.x);
                        sc.offset.x = 0;
                    }
                    if (sc.offset.y < 0)
                    {
                        sc.extent.height = (uint32_t)((int32_t)sc.extent.height + sc.offset.y);
                        sc.offset.y = 0;
                    }
                    if (sc.extent.width == 0 || sc.extent.height == 0 ||
                        (uint32_t)sc.offset.x >= extent_.width ||
                        (uint32_t)sc.offset.y >= extent_.height)
                        continue;   // 完全被裁掉：不画
                    if ((uint32_t)sc.offset.x + sc.extent.width > extent_.width)
                        sc.extent.width = extent_.width - (uint32_t)sc.offset.x;
                    if ((uint32_t)sc.offset.y + sc.extent.height > extent_.height)
                        sc.extent.height = extent_.height - (uint32_t)sc.offset.y;
                }
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // ⚠ 参数顺序是
                //   vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance)
                // 批次偏移选的是从哪个实例取属性，所以它是第五个参数。把它当
                // firstVertex 传会让驱动从第 18 个顶点开始读 6 顶点的四边形
                // 缓冲——已经越界——于是第一个批次之后所有批次都静默什么都不画，
                // 而命令缓冲、校验层和批次列表看起来全都正确。
                vkCmdDraw(cmd, 6, b.count, 0, b.first);
            }

            vkCmdEndRenderPass(cmd);
        };

        if (use_blur)
        {
            // pass 1：整个 UI 画进离屏（留下 COLOR_ATTACHMENT_OPTIMAL）
            draw_into(scene_fb_, scene_pass_, scene_pipe_);
            // pass 2：4 倍降采样 == 模糊（留下 SHADER_READ_ONLY）
            record_blur_pass(cmd);
            // pass 3：上屏，glass 采样模糊背景
            draw_into(swap_fb_[img], render_pass_, pipe_);
        }
        else
        {
            draw_into(swap_fb_[img], render_pass_, pipe_); // 快速路径
        }
        // fence 创建时是 SIGNALED，好让第一次 vkWaitForFences 不会永远阻塞。
        // 但提交要求它是 UNSIGNALED，所以交给 vkQueueSubmit 前要 reset——
        // 提交一个已 signal 的 fence 违反规范，有些驱动会静默丢掉工作。
        vkResetFences(device_, 1, &fence_);

        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_available_;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        // 不调用它的话录好的绘制调用永远不会被提交：命令缓冲停在 RECORDING
        // 状态，vkQueueSubmit 会拒绝它，帧只呈现一个清屏。看起来就像"着色器
        // 什么都没画"，而其他一切正常。
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            fault("vkEndCommandBuffer failed");
            return;
        }

        // 在属于这张图像的信号量上发信号和等待，这样还在读图像 N 的 present
        // 永远不会被用图像 N+1 的帧发信号。
        VkSemaphore sem_render = (img < sem_render_.size()) ? sem_render_[img] : VK_NULL_HANDLE;
        if (sem_render) si.signalSemaphoreCount = 1;
        if (sem_render) si.pSignalSemaphores = &sem_render;
        if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS)
        {
            // 这一帧在 GPU 上从未开始，没有东西可呈现。照旧呈现会去等一个
            // 永远不会被发信号的信号量。
            fault("vkQueueSubmit failed");
            return;
        }

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        if (sem_render)
        {
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &sem_render;
        }
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &img;
        // present 时的 OUT_OF_DATE / SUBOPTIMAL 是"resize 进行中"的正常结果，
        // 不算故障：下一次 acquire 会重建 swapchain。其他结果才表示帧没上屏。
        const VkResult pr = vkQueuePresentKHR(queue_, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR)
            fault("vkQueuePresentKHR failed");

        // ⚠ 不要在这里释放命令缓冲。fence 发信号前它还在执行中，释放一个
        //   pending 的缓冲是 VUID-vkFreeCommandBuffers-pCommandBuffers-00047
        //   ——驱动可能在 GPU 还在执行它时回收内存。池标志里有
        //   RESET_COMMAND_BUFFER_BIT，缓冲不释放也能复用；shutdown 时释放
        //   一次就够。
        ++frame_index_;
    }

    // ── 像素回读 ────────────────────────────────────────────────────
    //
    // "GPU 真的画了吗"的事实依据。窗口截图可能被遮挡、截在 resize 中途、
    // 或对着过时的合成器 surface，那样"UI 是空的"就成了无法证明的说法。
    // 从显存把真实图像拷出来，就把合成器彻底排除在问题之外。
    bool VkBackend::dump_ppm(const char *path)
    {
        if (!device_ || !swapchain_ || swap_images_.empty()) return false;

        const u32 W = extent_.width, H = extent_.height;
        if (!W || !H) return false;
        const VkDeviceSize bytes = (VkDeviceSize)W * H * 4;

        vkDeviceWaitIdle(device_);

        // ── 只 acquire 一张图像 ─────────────────────────────────────
        // swapchain 图像只允许在"返回它的 acquire"到"释放它的 present"之间
        // 触碰（UNASSIGNED-non-acquired-swapchain-image-used）。全读一遍也不
        // 行：surface 只保证在要求 present 之前可以 acquire minImageCount 次
        // （VUID-vkAcquireNextImageKHR-surface-07783）。
        //
        // 用 fence 而不是信号量：我们要立刻拿到图像，而 UINT64_MAX 只有在
        // surface 保证前进时才是合法超时，上一帧已经取走一张图后这个前提
        // 就不成立了。
        u32 idx = 0;
        VkFenceCreateInfo afi{};
        afi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence afence = VK_NULL_HANDLE;
        vkCreateFence(device_, &afi, nullptr, &afence);
        VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, 1000000000ull /* 1s */,
                                            VK_NULL_HANDLE, afence, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
        {
            vkDestroyFence(device_, afence, nullptr);
            return false;
        }
        vkWaitForFences(device_, 1, &afence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, afence, nullptr);

        VkImage img = swap_images_[idx];

        // ── 暂存缓冲 ────────────────────────────────────────────────
        VkBufferCreateInfo sbi{};
        sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        sbi.size = bytes;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer stage = VK_NULL_HANDLE;
        if (vkCreateBuffer(device_, &sbi, nullptr, &stage) != VK_SUCCESS)
        {
            VkPresentInfoKHR early{};
            early.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            early.swapchainCount = 1;
            early.pSwapchains = &swapchain_;
            early.pImageIndices = &idx;
            vkQueuePresentKHR(queue_, &early);
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, stage, &req);
        VkMemoryAllocateInfo sai{};
        sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        sai.allocationSize = req.size;
        sai.memoryTypeIndex = find_mem_type(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory smem = VK_NULL_HANDLE;
        bool ok = vkAllocateMemory(device_, &sai, nullptr, &smem) == VK_SUCCESS &&
                  vkBindBufferMemory(device_, stage, smem, 0) == VK_SUCCESS;

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cai, &cmd);

        // ── 录制拷贝 ────────────────────────────────────────────────
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &cbi);

        VkImageMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = img;
        mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &mb);

        VkBufferImageCopy bic{};
        bic.bufferOffset = 0;
        bic.bufferRowLength = 0; // 0 = 紧密排列，与 imageExtent.width 一致
        bic.bufferImageHeight = 0;
        bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bic.imageSubresource.mipLevel = 0;
        bic.imageSubresource.baseArrayLayer = 0;
        bic.imageSubresource.layerCount = 1;
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {W, H, 1};
        // regionCount 必须等于指针指向的 VkBufferImageCopy 结构体数量。
        // 多报会让驱动读过栈上局部变量的末尾，报出胡说的 extent 和 aspect。
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, 1, &bic);

        // 把图像原样交还，这样下面的 present 交出的就是普通帧那样的
        // PRESENT_SRC_KHR 图像。
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &mb);

        VkMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(cmd);

        // 给下面的 present 用的专用信号量。它必须由这次提交本身发信号，
        // 因为 VUID-vkQueuePresentKHR-pWaitSemaphores-03268 要求每个等待
        // 信号量都对应一个已提交的发信号操作——没人会发信号的信号量会被拒绝。
        VkSemaphoreCreateInfo psi{};
        psi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore psem = VK_NULL_HANDLE;
        vkCreateSemaphore(device_, &psi, nullptr, &psem);

        VkSubmitInfo si2{};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si2.commandBufferCount = 1;
        si2.pCommandBuffers = &cmd;
        if (psem)
        {
            si2.signalSemaphoreCount = 1;
            si2.pSignalSemaphores = &psem;
        }
        if (ok) vkQueueSubmit(queue_, 1, &si2, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);

        // ── 归还图像 ─────────────────────────────────────────────────
        //
        // 用专用信号量，不是 sem_render_[idx]。复用渲染路径的信号量正是
        // 按图像分信号量要避免的竞争：这次 present 背后没有 GPU 工作，
        // 对帧循环还持有的信号量发信号或等待会打乱它的记账。
        VkPresentInfoKHR dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        if (psem)
        {
            dpi.waitSemaphoreCount = 1;
            dpi.pWaitSemaphores = &psem;
        }
        dpi.swapchainCount = 1;
        dpi.pSwapchains = &swapchain_;
        dpi.pImageIndices = &idx;
        vkQueuePresentKHR(queue_, &dpi);

        // present 可能还在读信号量，所以要等队列真正空闲后才能销毁它。
        if (psem)
        {
            vkQueueWaitIdle(queue_);
            vkDestroySemaphore(device_, psem, nullptr);
        }

        std::vector<u8> px;
        if (ok)
        {
            void *mapped = nullptr;
            if (vkMapMemory(device_, smem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped)
            {
                px.assign((const u8 *)mapped, (const u8 *)mapped + bytes);
                vkUnmapMemory(device_, smem);
            }
        }
        vkDestroyBuffer(device_, stage, nullptr);
        vkFreeMemory(device_, smem, nullptr);
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);

        if (px.empty()) return false;

        // 统计与角颜色不同的像素数。健康的 UI 是几万量级；0 表示 GPU 什么
        // 都没画，不管窗口看起来什么样。
        const u8 bg[3] = {px[0], px[1], px[2]};
        size_t score = 0;
        for (size_t p = 0; p + 3 < px.size(); p += 4)
            if (px[p] != bg[0] || px[p + 1] != bg[1] || px[p + 2] != bg[2]) ++score;
        best_pixels_ = score;

        FILE *f = std::fopen(path, "wb");
        if (!f) return false;
        fprintf(f, "P6\n%u %u\n255\n", W, H);
        const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM ||
                          color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
        for (u32 y = 0; y < H; ++y)
        {
            const u8 *row = px.data() + (size_t)y * W * 4;
            for (u32 x = 0; x < W; ++x)
            {
                const u8 *p4 = row + (size_t)x * 4;
                fputc(bgra ? p4[2] : p4[0], f);
                fputc(p4[1], f);
                fputc(bgra ? p4[0] : p4[2], f);
            }
        }
        fclose(f);
        std::fprintf(stderr, "[Fox] dump_ppm -> %s (%zu px of %u differ from corner bg)\n",
                     path, score, W * H);
        return true;
    }
}