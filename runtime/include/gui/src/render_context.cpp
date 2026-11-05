#define NK_IMPLEMENTATION
#define NK_GLFW_VULKAN_IMPLEMENTATION
#include "render_context.h"
#include "font_loader.h"
#include "Nuklear/nuklear_glfw_vulkan.h"
#include <stdexcept>
#include <iostream>
#include <cstdio>

#define MAX_VERTEX_BUFFER (4 * 1024 * 1024)
#define MAX_INDEX_BUFFER (1 * 1024 * 1024)

namespace qgui
{

    static VkInstance create_vulkan_instance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Ravendrel Visualizer——夸克可视化器";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        uint32_t glfwExtensionCount = 0;
        const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        createInfo.enabledExtensionCount = glfwExtensionCount;
        createInfo.ppEnabledExtensionNames = glfwExtensions;

        VkInstance instance;
        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan instance");
        }
        return instance;
    }

    static VkPhysicalDevice pick_physical_device(VkInstance instance)
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            throw std::runtime_error("No Vulkan-capable GPU found");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        // 评分选卡：离散 GPU > 集成 GPU > 虚拟 GPU > CPU 软件渲染器
        auto typeScore = [](VkPhysicalDeviceType t) -> int {
            switch (t)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
            default: return 0; // CPU / 软件渲染
            }
        };

        VkPhysicalDevice best = devices[0];
        VkPhysicalDeviceProperties bestProps{};
        vkGetPhysicalDeviceProperties(best, &bestProps);
        int bestScore = typeScore(bestProps.deviceType);
        std::fprintf(stderr, "[GUI] Vulkan device[0]: %s (type %d)\n",
                     bestProps.deviceName, static_cast<int>(bestProps.deviceType));

        for (uint32_t i = 1; i < deviceCount; ++i)
        {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devices[i], &p);
            std::fprintf(stderr, "[GUI] Vulkan device[%u]: %s (type %d)\n",
                         i, p.deviceName, static_cast<int>(p.deviceType));
            int s = typeScore(p.deviceType);
            if (s > bestScore)
            {
                bestScore = s;
                best = devices[i];
                bestProps = p;
            }
        }
        std::fprintf(stderr, "[GUI] Using GPU: %s\n", bestProps.deviceName);
        return best;
    }

    static uint32_t find_graphics_queue_family(VkPhysicalDevice device)
    {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        for (uint32_t i = 0; i < queueFamilyCount; i++)
        {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                return i;
        }
        throw std::runtime_error("No Vulkan graphics queue family found");
    }

    static VkDevice create_logical_device(VkPhysicalDevice physicalDevice,
                                          uint32_t queueFamilyIndex,
                                          VkQueue &outQueue)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        float queuePriority = 1.0f;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceFeatures deviceFeatures{};

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;

        const std::vector<const char *> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        VkDevice device;
        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan logical device");
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &outQueue);
        return device;
    }

    bool RenderContext::create_swapchain()
    {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &modeCount, modes.data());

        present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
        for (auto m : modes)
        {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                present_mode_ = m;
                break;
            }
        }

        extent_ = caps.currentExtent;
        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        {
            imageCount = caps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = imageCount;
        ci.imageFormat = color_format_;
        ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent = extent_;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = present_mode_;
        ci.clipped = VK_TRUE;

        if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS)
        {
            return false;
        }

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        swapchain_images_.resize(count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());

        swapchain_image_views_.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swapchain_images_[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = color_format_;
            vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            vi.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &vi, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS)
            {
                return false;
            }
        }
        return true;
    }

    void RenderContext::destroy_swapchain()
    {
        if (!device_)
            return;
        for (auto view : swapchain_image_views_)
        {
            vkDestroyImageView(device_, view, nullptr);
        }
        swapchain_image_views_.clear();
        swapchain_images_.clear();
        if (swapchain_)
        {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    bool RenderContext::init(const char *title, int width, int height)
    {
        if (!glfwInit())
        {
            std::cerr << "[GUI] Failed to initialize GLFW\n";
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
        if (!window_)
        {
            glfwTerminate();
            return false;
        }

        try
        {
            instance_ = create_vulkan_instance();
            if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to create window surface");
            }
            physical_device_ = pick_physical_device(instance_);
            queue_family_ = find_graphics_queue_family(physical_device_);

            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, surface_, &presentSupport);
            if (!presentSupport)
            {
                throw std::runtime_error("Queue does not support presentation");
            }

            device_ = create_logical_device(physical_device_, queue_family_, queue_);

            if (!create_swapchain())
            {
                throw std::runtime_error("Failed to create swapchain");
            }

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &image_available_);

            ctx_ = nk_glfw3_init(window_, device_, physical_device_, queue_family_,
                                 swapchain_image_views_.data(),
                                 static_cast<uint32_t>(swapchain_image_views_.size()),
                                 color_format_, NK_GLFW3_INSTALL_CALLBACKS,
                                 MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER);
            if (!ctx_)
            {
                throw std::runtime_error("nk_glfw3_init returned null");
            }

            struct nk_font_atlas *atlas;
            nk_glfw3_font_stash_begin(&atlas);
            load_i18n_fonts(atlas); // 多字号 + CJK + 西里尔 + 拉丁
            nk_glfw3_font_stash_end(queue_);
            // 烘焙后回填字形数，供 rvdl_ui_atlas_info() 报。
            //
            // 多字号把显存开销抬了不少（3 档带 CJK ≈ 20 MB），
            // 必须能核对 —— 否则把 maxImageDimension2D 撑爆时只会看到「启动失败」。
            //
            // ⚠ 报字形数而不是图集宽高：nuklear 的 nk_font_atlas_bake
            //   确实会写 atlas->tex_width，但 font_stash_end 之后拿不到它，
            //   而早年那个「读 tex_width」的版本于是**恒返回 0** ——
            //   0 看起来像「图集是空的」，会把人引到完全错误的方向。
            qgui::set_atlas_glyphs(atlas->glyph_count);
            std::fprintf(stderr, "[Bro] font tiers=%d glyphs=%d\n",
                         qgui::font_count(), atlas->glyph_count);
            nk_style_default(ctx_);

            last_time_ = glfwGetTime();
            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[GUI] " << e.what() << "\n";
            shutdown();
            return false;
        }
    }

    void RenderContext::shutdown()
    {
        if (ctx_)
        {
            nk_glfw3_shutdown();
            ctx_ = nullptr;
        }
        if (image_available_ && device_)
        {
            vkDestroySemaphore(device_, image_available_, nullptr);
            image_available_ = VK_NULL_HANDLE;
        }
        destroy_swapchain();
        if (device_)
        {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ && instance_)
        {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_)
        {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (window_)
        {
            glfwDestroyWindow(window_);
            window_ = nullptr;
            glfwTerminate();
        }
    }

    bool RenderContext::should_close() const
    {
        return window_ == nullptr || glfwWindowShouldClose(window_);
    }

    void RenderContext::begin_frame()
    {
        glfwPollEvents();
        nk_glfw3_new_frame();
        double now = glfwGetTime();
        delta_time_ = static_cast<float>(now - last_time_);
        last_time_ = now;
    }

    void RenderContext::end_frame()
    {
        uint32_t imageIndex = 0;
        // 有限超时（250ms）：避免 semaphore 异常时无限死等导致「无响应」
        VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, 250000000ULL,
                                                 image_available_, VK_NULL_HANDLE,
                                                 &imageIndex);
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
            return;

        VkSemaphore renderFinished =
            nk_glfw3_render(queue_, imageIndex, image_available_, NK_ANTI_ALIASING_ON);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;
        vkQueuePresentKHR(queue_, &presentInfo);
        vkQueueWaitIdle(queue_);
    }
}