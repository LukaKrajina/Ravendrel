#pragma once
#include <vector>
#include <cstring>
#include <stdexcept>
#include "scene.hpp"
#include "vulkan_context.hpp"

namespace rvdlrsp::render {

    class GpuMesh {
    public:
        GpuMesh() = default;
        ~GpuMesh() { destroy(); }

        GpuMesh(const GpuMesh &) = delete;
        GpuMesh &operator=(const GpuMesh &) = delete;

        // 移动语义（供 std::vector 重新分配）
        GpuMesh(GpuMesh &&o) noexcept { move_from(o); }
        GpuMesh &operator=(GpuMesh &&o) noexcept {
            if (this != &o) { destroy(); move_from(o); }
            return *this;
        }

        // 上传 CPU 网格到 GPU（需 device 已初始化）
        void upload(VulkanContext &ctx, const Mesh &mesh) {
            destroy();
            device_ = ctx.device();
            index_count_ = static_cast<uint32_t>(mesh.indices.size());
            if (index_count_ == 0) return;

            VkDevice device = ctx.device();
            VkPhysicalDevice pd = ctx.physical_device();

            create_buffer(device, pd, mesh.vertices.size() * sizeof(Vertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          mesh.vertices.data(), vertex_buffer_, vertex_memory_);
            create_buffer(device, pd, mesh.indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          mesh.indices.data(), index_buffer_, index_memory_);
        }

        VkBuffer vertex_buffer() const { return vertex_buffer_; }
        VkBuffer index_buffer() const { return index_buffer_; }
        uint32_t index_count() const { return index_count_; }
        bool valid() const { return vertex_buffer_ != VK_NULL_HANDLE; }

        // 销毁 GPU 资源（使用创建时记录的 device）
        void destroy() {
            if (device_ == VK_NULL_HANDLE) return;
            if (vertex_buffer_ != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, vertex_buffer_, nullptr);
                vertex_buffer_ = VK_NULL_HANDLE;
            }
            if (vertex_memory_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_, vertex_memory_, nullptr);
                vertex_memory_ = VK_NULL_HANDLE;
            }
            if (index_buffer_ != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, index_buffer_, nullptr);
                index_buffer_ = VK_NULL_HANDLE;
            }
            if (index_memory_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_, index_memory_, nullptr);
                index_memory_ = VK_NULL_HANDLE;
            }
            index_count_ = 0;
        }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
        VkBuffer index_buffer_ = VK_NULL_HANDLE;
        VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
        uint32_t index_count_ = 0;

        void move_from(GpuMesh &o) noexcept {
            device_ = o.device_;
            vertex_buffer_ = o.vertex_buffer_;
            vertex_memory_ = o.vertex_memory_;
            index_buffer_ = o.index_buffer_;
            index_memory_ = o.index_memory_;
            index_count_ = o.index_count_;
            o.device_ = VK_NULL_HANDLE;
            o.vertex_buffer_ = VK_NULL_HANDLE;
            o.vertex_memory_ = VK_NULL_HANDLE;
            o.index_buffer_ = VK_NULL_HANDLE;
            o.index_memory_ = VK_NULL_HANDLE;
            o.index_count_ = 0;
        }

        static uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t type_filter,
                                         VkMemoryPropertyFlags props) {
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(pd, &mp);
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((type_filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                    return i;
            throw std::runtime_error("No suitable memory type for mesh");
        }

        static void create_buffer(VkDevice device, VkPhysicalDevice pd, VkDeviceSize size,
                                  VkBufferUsageFlags usage, const void *data,
                                  VkBuffer &buffer, VkDeviceMemory &memory) {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = size;
            bi.usage = usage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bi, nullptr, &buffer) != VK_SUCCESS)
                throw std::runtime_error("Failed to create buffer");

            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(device, buffer, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = find_memory_type(pd, mr.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
                throw std::runtime_error("Failed to allocate buffer memory");
            vkBindBufferMemory(device, buffer, memory, 0);

            void *mapped;
            vkMapMemory(device, memory, 0, size, 0, &mapped);
            std::memcpy(mapped, data, static_cast<size_t>(size));
            vkUnmapMemory(device, memory);
        }
    };
}