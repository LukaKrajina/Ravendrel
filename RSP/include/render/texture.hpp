#pragma once
#include <cstring>
#include <stdexcept>
#include "vulkan_context.hpp"
#include "texture_decoder.hpp"

namespace rvdlrsp::render
{

    class VulkanTexture
    {
    private:
        VulkanContext *ctx_ = nullptr;
        VkImage image_ = VK_NULL_HANDLE;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
        VkImageView view_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        int width_ = 0, height_ = 0;

    public:
        VulkanTexture() = default;
        ~VulkanTexture() { destroy(); }

        VulkanTexture(const VulkanTexture &) = delete;
        VulkanTexture &operator=(const VulkanTexture &) = delete;

        VulkanTexture(VulkanTexture &&o) noexcept
        {
            ctx_ = o.ctx_;
            image_ = o.image_;
            memory_ = o.memory_;
            view_ = o.view_;
            sampler_ = o.sampler_;
            width_ = o.width_;
            height_ = o.height_;
            o.ctx_ = nullptr;
            o.image_ = VK_NULL_HANDLE;
            o.memory_ = VK_NULL_HANDLE;
            o.view_ = VK_NULL_HANDLE;
            o.sampler_ = VK_NULL_HANDLE;
        }
        VulkanTexture &operator=(VulkanTexture &&o) noexcept
        {
            if (this != &o)
            {
                destroy();
                ctx_ = o.ctx_;
                image_ = o.image_;
                memory_ = o.memory_;
                view_ = o.view_;
                sampler_ = o.sampler_;
                width_ = o.width_;
                height_ = o.height_;
                o.ctx_ = nullptr;
                o.image_ = VK_NULL_HANDLE;
                o.memory_ = VK_NULL_HANDLE;
                o.view_ = VK_NULL_HANDLE;
                o.sampler_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // 上传 RGBA8 图像到 GPU
        void upload(VulkanContext &ctx, const DecodedImage &img)
        {
            if (!img.valid || img.pixels.empty())
                return;
            destroy();
            ctx_ = &ctx;
            width_ = img.width;
            height_ = img.height;

            VkDevice device = ctx.device();

            // 1. 创建 VkImage（LINEAR tiling，host-visible）
            VkImageCreateInfo ii{};
            ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = {static_cast<uint32_t>(img.width),
                         static_cast<uint32_t>(img.height), 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_LINEAR;
            ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
            if (vkCreateImage(device, &ii, nullptr, &image_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create texture image");

            // 2. 分配 host-visible 内存
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(device, image_, &mr);

            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(ctx.physical_device(), &mp);
            uint32_t mem_type = 0;
            bool found = false;
            for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
                if ((mr.memoryTypeBits & (1 << t)) &&
                    (mp.memoryTypes[t].propertyFlags &
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
                {
                    mem_type = t;
                    found = true;
                    break;
                }
            if (!found)
                throw std::runtime_error("No host-visible memory for texture");

            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = mem_type;
            if (vkAllocateMemory(device, &ai, nullptr, &memory_) != VK_SUCCESS)
                throw std::runtime_error("Failed to allocate texture memory");
            vkBindImageMemory(device, image_, memory_, 0);

            // 3. 映射并上传像素（直接按行拷贝）
            VkImageSubresource sub{};
            sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sub.mipLevel = 0;
            sub.arrayLayer = 0;
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device, image_, &sub, &layout);

            void *mapped;
            vkMapMemory(device, memory_, 0, mr.size, 0, &mapped);
            uint8_t *dst = static_cast<uint8_t *>(mapped);
            for (int y = 0; y < img.height; ++y)
            {
                std::memcpy(dst + y * layout.rowPitch,
                            img.pixels.data() + static_cast<size_t>(y) * img.width * 4,
                            static_cast<size_t>(img.width) * 4);
            }
            vkUnmapMemory(device, memory_);

            // 4. 创建 ImageView
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = image_;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device, &vi, nullptr, &view_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create texture view");

            // 5. 创建 Sampler
            VkSamplerCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter = VK_FILTER_LINEAR;
            si.minFilter = VK_FILTER_LINEAR;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            if (vkCreateSampler(device, &si, nullptr, &sampler_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create texture sampler");
        }

        VkImageView view() const { return view_; }
        VkSampler sampler() const { return sampler_; }
        int width() const { return width_; }
        int height() const { return height_; }
        bool valid() const { return image_ != VK_NULL_HANDLE; }

        void destroy()
        {
            if (!ctx_)
                return;
            VkDevice device = ctx_->device();
            if (sampler_)
                vkDestroySampler(device, sampler_, nullptr);
            if (view_)
                vkDestroyImageView(device, view_, nullptr);
            if (image_)
                vkDestroyImage(device, image_, nullptr);
            if (memory_)
                vkFreeMemory(device, memory_, nullptr);
            sampler_ = VK_NULL_HANDLE;
            view_ = VK_NULL_HANDLE;
            image_ = VK_NULL_HANDLE;
            memory_ = VK_NULL_HANDLE;
        }
    };
}