#include "render/vulkan_context.hpp"

#include <array>

namespace rvdlrsp::render {

    static VkInstance create_instance() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "rvdlRSP";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_0;

        uint32_t ext_count = 0;
        const char **exts = glfwGetRequiredInstanceExtensions(&ext_count);

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = ext_count;
        ci.ppEnabledExtensionNames = exts;

        VkInstance inst;
        if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS)
            throw std::runtime_error("Failed to create Vulkan instance");
        return inst;
    }

    static VkPhysicalDevice pick_physical_device(VkInstance instance) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) throw std::runtime_error("No Vulkan GPU");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        return devices[0];
    }

    static uint32_t find_graphics_queue(VkPhysicalDevice device) {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, qfs.data());
        for (uint32_t i = 0; i < count; ++i)
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
        throw std::runtime_error("No graphics queue family");
    }

    static VkDevice create_device(VkPhysicalDevice pd, uint32_t qf, VkQueue &queue) {
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        float prio = 1.0f;
        qci.pQueuePriorities = &prio;

        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.pQueueCreateInfos = &qci;
        ci.queueCreateInfoCount = 1;
        ci.pEnabledFeatures = &features;
        const std::vector<const char *> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();

        VkDevice dev;
        if (vkCreateDevice(pd, &ci, nullptr, &dev) != VK_SUCCESS)
            throw std::runtime_error("Failed to create logical device");
        vkGetDeviceQueue(dev, qf, 0, &queue);
        return dev;
    }

    uint32_t VulkanContext::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((type_filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        throw std::runtime_error("No suitable memory type");
    }

    bool VulkanContext::init(const char *title, int width, int height) {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
        if (!window_) { glfwTerminate(); return false; }

        try {
            instance_ = create_instance();
            if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create surface");
            physical_device_ = pick_physical_device(instance_);
            queue_family_ = find_graphics_queue(physical_device_);
            device_ = create_device(physical_device_, queue_family_, queue_);

            if (!create_swapchain()) throw std::runtime_error("Failed to create swapchain");
            create_depth_resources();
            create_render_pass();
            create_framebuffers();
            create_command_buffers();
            create_sync_objects();
            return true;
        } catch (const std::exception &e) {
            std::cerr << "[rvdlRSP.render] " << e.what() << "\n";
            shutdown();
            return false;
        }
    }

    bool VulkanContext::create_swapchain() {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

        uint32_t mc = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mc, nullptr);
        std::vector<VkPresentModeKHR> modes(mc);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mc, modes.data());
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; }

        extent_ = caps.currentExtent;
        uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = count;
        ci.imageFormat = color_format_;
        ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent = extent_;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = pm;
        ci.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS) return false;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        swapchain_images_.resize(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchain_images_.data());

        swapchain_image_views_.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swapchain_images_[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = color_format_;
            vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &vi, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) return false;
        }
        return true;
    }

    void VulkanContext::create_depth_resources() {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.extent = {extent_.width, extent_.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.format = VK_FORMAT_D32_SFLOAT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ii, nullptr, &depth_image_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create depth image");

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(device_, depth_image_, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &depth_memory_) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate depth memory");
        vkBindImageMemory(device_, depth_image_, depth_memory_, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = depth_image_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &vi, nullptr, &depth_view_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create depth view");
    }

    void VulkanContext::create_render_pass() {
        VkAttachmentDescription color{};
        color.format = color_format_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depth{};
        depth.format = VK_FORMAT_D32_SFLOAT;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color_ref;
        sub.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        std::array<VkAttachmentDescription, 2> atts = {color, depth};
        VkRenderPassCreateInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = static_cast<uint32_t>(atts.size());
        rpi.pAttachments = atts.data();
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies = &dep;
        if (vkCreateRenderPass(device_, &rpi, nullptr, &render_pass_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create render pass");
    }

    void VulkanContext::create_framebuffers() {
        framebuffers_.resize(swapchain_image_views_.size());
        for (size_t i = 0; i < swapchain_image_views_.size(); ++i) {
            std::array<VkImageView, 2> atts = {swapchain_image_views_[i], depth_view_};
            VkFramebufferCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = render_pass_;
            fi.attachmentCount = static_cast<uint32_t>(atts.size());
            fi.pAttachments = atts.data();
            fi.width = extent_.width;
            fi.height = extent_.height;
            fi.layers = 1;
            if (vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[i]) != VK_SUCCESS)
                throw std::runtime_error("Failed to create framebuffer");
        }
    }

    void VulkanContext::create_command_buffers() {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = queue_family_;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device_, &pi, nullptr, &command_pool_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create command pool");

        command_buffers_.resize(swapchain_image_views_.size());
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = command_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());
        if (vkAllocateCommandBuffers(device_, &ai, command_buffers_.data()) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate command buffers");
    }

    void VulkanContext::create_sync_objects() {
        image_available_.resize(MAX_FRAMES_IN_FLIGHT);
        render_finished_.resize(MAX_FRAMES_IN_FLIGHT);
        in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            vkCreateSemaphore(device_, &si, nullptr, &image_available_[i]);
            vkCreateSemaphore(device_, &si, nullptr, &render_finished_[i]);
            vkCreateFence(device_, &fi, nullptr, &in_flight_fences_[i]);
        }
    }

    VkCommandBuffer VulkanContext::begin_frame(uint32_t &out_image_index) {
        vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);

        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                           image_available_[current_frame_], VK_NULL_HANDLE,
                                           &out_image_index);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return VK_NULL_HANDLE;

        vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);

        VkCommandBuffer cmd = command_buffers_[out_image_index];
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);
        return cmd;
    }

    void VulkanContext::end_frame(uint32_t image_index, VkCommandBuffer cmd) {
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &image_available_[current_frame_];
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &render_finished_[current_frame_];
        vkQueueSubmit(queue_, 1, &si, in_flight_fences_[current_frame_]);

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_finished_[current_frame_];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &image_index;
        vkQueuePresentKHR(queue_, &pi);

        current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void VulkanContext::destroy_swapchain() {
        if (device_ == VK_NULL_HANDLE) return;
        for (auto v : swapchain_image_views_) vkDestroyImageView(device_, v, nullptr);
        swapchain_image_views_.clear();
        if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    }

    void VulkanContext::destroy_depth_resources() {
        if (device_ == VK_NULL_HANDLE) return;
        if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
        if (depth_image_) vkDestroyImage(device_, depth_image_, nullptr);
        if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
    }

    void VulkanContext::destroy_framebuffers() {
        if (device_ == VK_NULL_HANDLE) return;
        for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
        framebuffers_.clear();
    }

    void VulkanContext::shutdown() {
        if (device_ == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(device_);

        for (auto s : image_available_) vkDestroySemaphore(device_, s, nullptr);
        for (auto s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
        for (auto f : in_flight_fences_) vkDestroyFence(device_, f, nullptr);

        if (command_pool_) { vkDestroyCommandPool(device_, command_pool_, nullptr); command_pool_ = VK_NULL_HANDLE; }

        destroy_framebuffers();
        if (render_pass_) { vkDestroyRenderPass(device_, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
        destroy_depth_resources();
        destroy_swapchain();

        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        if (surface_ && instance_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
        if (window_) { glfwDestroyWindow(window_); window_ = nullptr; glfwTerminate(); }
    }
}