#include "vk_backend.hpp"
#include "shaders_spv.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace fox
{
    static void scale_instance(InstanceData &d, f32 s)
    {
        for (int i = 0; i < 4; ++i)
        {
            d.rect[i] *= s;
            d.radii[i] *= s;
        }
        d.params[1] *= s;
        d.params[2] *= s;

        const f32 type = d.params[0];
        if (type == PT_PATH)
        {
            d.params2[0] *= s;
            d.params2[1] *= s;
        }
        else if (type == PT_TRIANGLE)
        {
            d.params[3]  *= s;
            d.params2[0] *= s;
            d.params2[1] *= s;
            d.params2[2] *= s;
        }
    }



    static float backend_scale();

    double VkBackend::g_scroll_x_ = 0.0;
    double VkBackend::g_scroll_y_ = 0.0;
    std::vector<u32> VkBackend::g_typed_chars;


    u32 VkBackend::find_mem_type(u32 bits, VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        for (u32 i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        return UINT32_MAX;
    }


    bool VkBackend::init(GLFWwindow *window, Context &ctx)
    {
        window_ = window;
        ctx_ = &ctx;
        try
        {
            if (!create_instance()) { std::fprintf(stderr, "[Fox] create_instance failed\n"); return false; }
            std::fprintf(stderr, "[Fox] instance ok\n");
            if (!create_device())   { std::fprintf(stderr, "[Fox] create_device failed\n"); return false; }
            std::fprintf(stderr, "[Fox] device ok\n");

            int w = 960, h = 640;
            glfwGetFramebufferSize(window_, &w, &h);
            if (w <= 0) w = 960;
            if (h <= 0) h = 640;

            float sc = backend_scale();
            if (sc > 0.0f) ctx_->set_scale(sc);
            ctx_->set_framebuffer_size(w, h);

            if (!create_swapchain()) { std::fprintf(stderr, "[Fox] create_swapchain failed\n"); return false; }
            if (!create_geometry())  { std::fprintf(stderr, "[Fox] create_geometry failed\n"); return false; }
            std::fprintf(stderr, "[Fox] geometry ok\n");
            if (!create_pipeline())  { std::fprintf(stderr, "[Fox] create_pipeline failed\n"); return false; }
            std::fprintf(stderr, "[Fox] pipeline ok\n");
            if (!create_sampler())   { std::fprintf(stderr, "[Fox] create_sampler failed\n"); return false; }
            if (!create_descriptors()){ std::fprintf(stderr, "[Fox] create_descriptors failed\n"); return false; }

            glfwSetScrollCallback(window_, [](GLFWwindow *, double x, double y) {
                g_scroll_x_ += x;
                g_scroll_y_ += y;
            });
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


    bool VkBackend::create_swapchain()
    {
        std::fprintf(stderr, "[Fox] swapchain: caps...\n");
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
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


        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &si, nullptr, &sem_available_);

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


    static float backend_scale()
    {
        if (const char *e = getenv("FOX_SCALE"))
        {
            const float v = (float)atof(e);
            if (v > 0.0f) return v;
        }
        return 0.0f;
    }

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
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.maxLod = 16.0f;
        return vkCreateSampler(device_, &ci, nullptr, &sampler_) == VK_SUCCESS;
    }


    bool VkBackend::create_pipeline()
    {
        VkShaderModuleCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vci.codeSize = FOX_SPV_VERT_SIZE;
        vci.pCode = fox_spv_vert;
        VkShaderModule vs = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &vci, nullptr, &vs) != VK_SUCCESS) return false;
        vci.codeSize = FOX_SPV_FRAG_SIZE;
        vci.pCode = fox_spv_frag;
        VkShaderModule fs = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &vci, nullptr, &fs) != VK_SUCCESS) return false;

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
        vbinds[0].stride = 8;
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

        VkAttachmentDescription att{};
        att.format = color_format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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


    FontId VkBackend::slot_of(FontId f) const
    {
        for (size_t i = 0; i < font_slots_.size(); ++i)
            if (font_slots_[i].id == f) return (FontId)(i + 1);
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
        if (!desc_pool_) return -1;
        font_slots_.push_back(FontSlot{});
        font_slots_.back().id = f;
        if (!rebuild_desc_sets())
        {
            font_slots_.pop_back();
            return -1;
        }
        return (int)font_slots_.size() - 1;
    }

    bool VkBackend::rebuild_desc_sets()
    {
        if (!set_layout_) return false;
        const size_t want = 1 + font_slots_.size() + tex_slots_.size();
        const u32 sets = (u32)want;

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

    void VkBackend::write_descriptors()
    {
        VkDescriptorBufferInfo dbi{ubo_, 0, 16};

        if (!sampler_) return;
        if (!dummy_view_ && !create_dummy_image()) return;

        VkImageView safe_blur = blur_view_ ? blur_view_ : dummy_view_;
        if (!safe_blur) return;

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

            VkImageView tv = dummy_view_;
            if (i >= tex_base && (i - tex_base) < tex_slots_.size())
            {
                const TexSlot &ts = tex_slots_[i - tex_base];
                if (ts.view && ts.uploaded) tv = ts.view;
            }

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
        if (!cmd_pool_) return false;
        if (!make_image(1, 1, VK_FORMAT_R8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, dummy_img_,
                        dummy_mem_, dummy_view_))
            return false;

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
        bi.size = 16;
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


        if (!rebuild_desc_sets()) return false;
        return true;
    }


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
        const u32 levels = src.mip_levels();

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
            if (!make_image(src.w, src.h, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ts.img,
                            ts.mem, ts.view, levels))
            {
                return false;
            }
            ts.w = src.w;
            ts.h = src.h;
            ts.mip_levels = levels;
        }
        if (!ts.img || !ts.view) return false;

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

        const size_t want = 1 + font_slots_.size() + tex_slots_.size();
        if (desc_sets_.size() != want)
        {
            if (!rebuild_desc_sets()) return;
            need_desc = true;
        }

        if (need_desc || need_slot) write_descriptors();
    }


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
        if (!make_image(w, h, color_format_,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        scene_img_, scene_mem_, scene_view_))
            return false;
        scene_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

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
        blur_img_created_ = false;
        blur_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        scene_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        write_descriptors();
    }


    bool VkBackend::upload_atlas(FontId id)
    {
        GlyphAtlas *atlas = fonts().atlas(id);
        if (!atlas) return false;
        int w = atlas->width(), h = atlas->height();
        if (w <= 0 || h <= 0) return false;
        const std::vector<u8> &px = atlas->pixels();

        int si = ensure_slot(id);
        if (si < 0) return false;
        FontSlot &sl = font_slots_[(size_t)si];

        if (!sl.img)
        {
            make_image((u32)w, (u32)h, VK_FORMAT_R8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sl.img,
                       sl.mem, sl.view);
            sl.uploaded = false;
        }

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



    struct EdgeTracker
    {
        bool mouse_down[3] = {false, false, false};
        bool key_down[KEY_SLOTS] = {false};
    };
    static EdgeTracker g_edges;

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

        in.chars = g_typed_chars;
        g_typed_chars.clear();

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


    void VkBackend::record_blur_pass(VkCommandBuffer cmd)
    {
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

        barrier(scene_img_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        scene_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAccessFlags src = 0;
        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (blur_layout_ == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            src = 0;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        }
        else
        {
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
        blur_binding_stale_ = true;
    }

    void VkBackend::render(const DisplayList &dl, Context &ctx)
    {
        if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        {
            fault("vkWaitForFences failed (device lost?)");
            return;
        }

        sync_textures();

        build_instances(ctx, dl, frame_);
        last_instances_ = (u32)frame_.instances.size();
        last_used_blur_ = frame_.used_glass;
        last_dropped_ = frame_.dropped;
        batches_ = frame_.batches;
        if (getenv("FOX_TRACE"))
            std::fprintf(stderr, "[Fox] build_instances: %u instances, %zu batches, %u dropped\n",
                         last_instances_, batches_.size(), frame_.dropped);

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

        const u32 want_div = ctx.quality().blur == 1 ? 8u : 4u;
        if (blur_div_ != want_div)
        {
            destroy_offscreen();
            blur_div_ = want_div;
        }

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

            long current = -2;
            for (const Batch &b : batches_)
            {
                if (b.count == 0) continue;

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
                VkRect2D sc{{0, 0}, extent_};
                if (b.clip.w > 0.0f && b.clip.h > 0.0f)
                {
                    sc.offset.x = (int32_t)(b.clip.x * dpi);
                    sc.offset.y = (int32_t)(b.clip.y * dpi);
                    sc.extent.width = (uint32_t)(b.clip.w * dpi);
                    sc.extent.height = (uint32_t)(b.clip.h * dpi);
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
                        continue;
                    if ((uint32_t)sc.offset.x + sc.extent.width > extent_.width)
                        sc.extent.width = extent_.width - (uint32_t)sc.offset.x;
                    if ((uint32_t)sc.offset.y + sc.extent.height > extent_.height)
                        sc.extent.height = extent_.height - (uint32_t)sc.offset.y;
                }
                vkCmdSetScissor(cmd, 0, 1, &sc);

                vkCmdDraw(cmd, 6, b.count, 0, b.first);
            }

            vkCmdEndRenderPass(cmd);
        };

        if (use_blur)
        {
            draw_into(scene_fb_, scene_pass_, scene_pipe_);
            record_blur_pass(cmd);
            draw_into(swap_fb_[img], render_pass_, pipe_);
        }
        else
        {
            draw_into(swap_fb_[img], render_pass_, pipe_);
        }
        vkResetFences(device_, 1, &fence_);

        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_available_;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            fault("vkEndCommandBuffer failed");
            return;
        }

        VkSemaphore sem_render = (img < sem_render_.size()) ? sem_render_[img] : VK_NULL_HANDLE;
        if (sem_render) si.signalSemaphoreCount = 1;
        if (sem_render) si.pSignalSemaphores = &sem_render;
        if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS)
        {
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
        const VkResult pr = vkQueuePresentKHR(queue_, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR)
            fault("vkQueuePresentKHR failed");

        ++frame_index_;
    }

    bool VkBackend::dump_ppm(const char *path)
    {
        if (!device_ || !swapchain_ || swap_images_.empty()) return false;

        const u32 W = extent_.width, H = extent_.height;
        if (!W || !H) return false;
        const VkDeviceSize bytes = (VkDeviceSize)W * H * 4;

        vkDeviceWaitIdle(device_);

        u32 idx = 0;
        VkFenceCreateInfo afi{};
        afi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence afence = VK_NULL_HANDLE;
        vkCreateFence(device_, &afi, nullptr, &afence);
        VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, 1000000000ull ,
                                            VK_NULL_HANDLE, afence, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
        {
            vkDestroyFence(device_, afence, nullptr);
            return false;
        }
        vkWaitForFences(device_, 1, &afence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, afence, nullptr);

        VkImage img = swap_images_[idx];

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
        bic.bufferRowLength = 0;
        bic.bufferImageHeight = 0;
        bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bic.imageSubresource.mipLevel = 0;
        bic.imageSubresource.baseArrayLayer = 0;
        bic.imageSubresource.layerCount = 1;
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, 1, &bic);

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