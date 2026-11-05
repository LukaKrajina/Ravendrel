#pragma once
#include <vector>
#include <array>
#include <string>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include "mat4.hpp"
#include "mesh.hpp"

namespace rvdlrsp::render
{

    // Uniform：MVP + 光照 + PBR
    struct alignas(16) UboData
    {
        Mat4 mvp;              // 64 字节
        Mat4 model;            // 64 字节（世界空间变换，用于法线）
        float light_dir[3];    // 12 字节
        float light_intensity; // 4 字节
        float cam_pos[3];      // 12 字节
        float _pad0;           // 4 字节对齐
        float base_color[3];   // 12 字节（PBR）
        float metallic;        // 4 字节（PBR）
        float roughness;       // 4 字节（PBR）
        float use_texture;     // 4 字节（0=无纹理，1=采样 baseColor 纹理）
    };

    static std::vector<char> load_spirv(const std::string &path)
    {
        std::ifstream f(path, std::ios::ate | std::ios::binary);
        if (!f)
            throw std::runtime_error("Failed to open shader: " + path);
        size_t size = static_cast<size_t>(f.tellg());
        std::vector<char> buf(size);
        f.seekg(0);
        f.read(buf.data(), static_cast<std::streamsize>(size));
        return buf;
    }

    static VkShaderModule create_shader_module(VkDevice device, const std::vector<char> &code)
    {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
        VkShaderModule mod;
        if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
            throw std::runtime_error("Failed to create shader module");
        return mod;
    }

    class Pipeline3D
    {
    public:
        Pipeline3D() = default;
        ~Pipeline3D() { destroy(); }

        Pipeline3D(const Pipeline3D &) = delete;
        Pipeline3D &operator=(const Pipeline3D &) = delete;

        void init(VulkanContext &ctx, const std::string &vert_spv, const std::string &frag_spv)
        {
            ctx_ = &ctx;
            VkDevice device = ctx.device();

            auto vert_code = load_spirv(vert_spv);
            auto frag_code = load_spirv(frag_spv);
            VkShaderModule vert = create_shader_module(device, vert_code);
            VkShaderModule frag = create_shader_module(device, frag_code);

            // 顶点输入：pos / normal / color
            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::vector<VkVertexInputAttributeDescription> attrs(4);
            attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
            attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
            attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)};
            attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, r)};

            VkPipelineVertexInputStateCreateInfo vis{};
            vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vis.vertexBindingDescriptionCount = 1;
            vis.pVertexBindingDescriptions = &binding;
            vis.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
            vis.pVertexAttributeDescriptions = attrs.data();

            VkPipelineInputAssemblyStateCreateInfo ias{};
            ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vps{};
            vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vps.viewportCount = 1;
            vps.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_BACK_BIT;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState cb{};
            cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo cbs{};
            cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cbs.attachmentCount = 1;
            cbs.pAttachments = &cb;

            std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dys{};
            dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dys.dynamicStateCount = static_cast<uint32_t>(dyn.size());
            dys.pDynamicStates = dyn.data();

            // 描述符集布局：binding 0 = uniform buffer，binding 1 = baseColor 纹理采样器
            std::array<VkDescriptorSetLayoutBinding, 2> lbs{};
            lbs[0].binding = 0;
            lbs[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            lbs[0].descriptorCount = 1;
            lbs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            lbs[1].binding = 1;
            lbs[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            lbs[1].descriptorCount = 1;
            lbs[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo dsl{};
            dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dsl.bindingCount = static_cast<uint32_t>(lbs.size());
            dsl.pBindings = lbs.data();
            if (vkCreateDescriptorSetLayout(device, &dsl, nullptr, &descriptor_layout_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create descriptor set layout");

            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &descriptor_layout_;
            if (vkCreatePipelineLayout(device, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create pipeline layout");

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            stages[1].pName = "main";

            VkGraphicsPipelineCreateInfo pi{};
            pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pi.stageCount = 2;
            pi.pStages = stages;
            pi.pVertexInputState = &vis;
            pi.pInputAssemblyState = &ias;
            pi.pViewportState = &vps;
            pi.pRasterizationState = &rs;
            pi.pMultisampleState = &ms;
            pi.pDepthStencilState = &ds;
            pi.pColorBlendState = &cbs;
            pi.pDynamicState = &dys;
            pi.layout = pipeline_layout_;
            pi.renderPass = ctx.render_pass();
            pi.subpass = 0;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create graphics pipeline");

            vkDestroyShaderModule(device, vert, nullptr);
            vkDestroyShaderModule(device, frag, nullptr);

            create_uniform_buffer(ctx);
        }

        void bind(VkCommandBuffer cmd, uint32_t image_index)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

            VkViewport vp{};
            vp.width = static_cast<float>(ctx_->extent().width);
            vp.height = static_cast<float>(ctx_->extent().height);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D sc{};
            sc.extent = ctx_->extent();
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    0, 1, &descriptor_sets_[image_index], 0, nullptr);
        }

        // 仅重绑描述符集
        // 纹理切换后调用，没必要重复绑定 pipeline/viewport/scissor
        void bind_descriptor(VkCommandBuffer cmd, uint32_t image_index)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    0, 1, &descriptor_sets_[image_index], 0, nullptr);
        }

        // 更新该帧的 MVP + 光照 + PBR 材质
        void update_uniform(uint32_t image_index, const Mat4 &mvp, const Mat4 &model,
                            const float light_dir[3], float intensity,
                            const float cam_pos[3], const Material &mat)
        {
            UboData ubo{};
            ubo.mvp = mvp;
            ubo.model = model;
            ubo.light_dir[0] = light_dir[0];
            ubo.light_dir[1] = light_dir[1];
            ubo.light_dir[2] = light_dir[2];
            ubo.light_intensity = intensity;
            ubo.cam_pos[0] = cam_pos[0];
            ubo.cam_pos[1] = cam_pos[1];
            ubo.cam_pos[2] = cam_pos[2];
            ubo.base_color[0] = mat.base_color[0];
            ubo.base_color[1] = mat.base_color[1];
            ubo.base_color[2] = mat.base_color[2];
            ubo.metallic = mat.metallic;
            ubo.roughness = mat.roughness;
            ubo.use_texture = mat.base_color_texture.valid ? 1.0f : 0.0f;

            void *mapped;
            vkMapMemory(ctx_->device(), uniform_memory_[image_index], 0, sizeof(UboData), 0, &mapped);
            std::memcpy(mapped, &ubo, sizeof(UboData));
            vkUnmapMemory(ctx_->device(), uniform_memory_[image_index]);
        }

        // 绑定 baseColor 纹理到 descriptor set（binding 1）
        void bind_texture(uint32_t image_index, VkSampler sampler, VkImageView view)
        {
            VkDescriptorImageInfo ii{};
            ii.sampler = sampler;
            ii.imageView = view;
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = descriptor_sets_[image_index];
            w.dstBinding = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1;
            w.pImageInfo = &ii;
            vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
        }

        // 访问描述符集布局（供 Renderer 创建默认纹理使用）
        VkDescriptorSetLayout descriptor_layout() const { return descriptor_layout_; }

        void destroy()
        {
            if (!ctx_)
                return;
            VkDevice device = ctx_->device();
            if (device == VK_NULL_HANDLE)
            {
                ctx_ = nullptr;
                return;
            }
            vkDeviceWaitIdle(device);

            if (pipeline_ != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(device, pipeline_, nullptr);
                pipeline_ = VK_NULL_HANDLE;
            }
            if (pipeline_layout_ != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
                pipeline_layout_ = VK_NULL_HANDLE;
            }
            for (auto b : uniform_buffers_)
                if (b != VK_NULL_HANDLE)
                    vkDestroyBuffer(device, b, nullptr);
            uniform_buffers_.clear();
            for (auto m : uniform_memory_)
                if (m != VK_NULL_HANDLE)
                    vkFreeMemory(device, m, nullptr);
            uniform_memory_.clear();
            descriptor_sets_.clear();
            if (descriptor_pool_ != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
                descriptor_pool_ = VK_NULL_HANDLE;
            }
            if (descriptor_layout_ != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device, descriptor_layout_, nullptr);
                descriptor_layout_ = VK_NULL_HANDLE;
            }
            ctx_ = nullptr;
        }

    private:
        VulkanContext *ctx_ = nullptr;
        VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
        std::vector<VkBuffer> uniform_buffers_;
        std::vector<VkDeviceMemory> uniform_memory_;
        std::vector<VkDescriptorSet> descriptor_sets_;

        void create_uniform_buffer(VulkanContext &ctx)
        {
            VkDevice device = ctx.device();
            uint32_t n = ctx.image_count();
            uniform_buffers_.resize(n);
            uniform_memory_.resize(n);
            descriptor_sets_.resize(n);

            std::array<VkDescriptorPoolSize, 2> pool_sizes{};
            pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            pool_sizes[0].descriptorCount = n;
            pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pool_sizes[1].descriptorCount = n;

            VkDescriptorPoolCreateInfo dpi{};
            dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpi.maxSets = n;
            dpi.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
            dpi.pPoolSizes = pool_sizes.data();
            if (vkCreateDescriptorPool(device, &dpi, nullptr, &descriptor_pool_) != VK_SUCCESS)
                throw std::runtime_error("Failed to create descriptor pool");

            std::vector<VkDescriptorSetLayout> layouts(n, descriptor_layout_);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = descriptor_pool_;
            ai.descriptorSetCount = n;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &ai, descriptor_sets_.data()) != VK_SUCCESS)
                throw std::runtime_error("Failed to allocate descriptor sets");

            for (uint32_t i = 0; i < n; ++i)
            {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = sizeof(UboData);
                bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                if (vkCreateBuffer(device, &bi, nullptr, &uniform_buffers_[i]) != VK_SUCCESS)
                    throw std::runtime_error("Failed to create uniform buffer");

                VkMemoryRequirements mr;
                vkGetBufferMemoryRequirements(device, uniform_buffers_[i], &mr);
                VkPhysicalDeviceMemoryProperties mp;
                vkGetPhysicalDeviceMemoryProperties(ctx.physical_device(), &mp);
                uint32_t mem_type = 0;
                for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
                    if ((mr.memoryTypeBits & (1 << t)) &&
                        (mp.memoryTypes[t].propertyFlags &
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
                    {
                        mem_type = t;
                        break;
                    }

                VkMemoryAllocateInfo mai{};
                mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                mai.allocationSize = mr.size;
                mai.memoryTypeIndex = mem_type;
                if (vkAllocateMemory(device, &mai, nullptr, &uniform_memory_[i]) != VK_SUCCESS)
                    throw std::runtime_error("Failed to allocate uniform memory");
                vkBindBufferMemory(device, uniform_buffers_[i], uniform_memory_[i], 0);

                VkDescriptorBufferInfo dbi{};
                dbi.buffer = uniform_buffers_[i];
                dbi.offset = 0;
                dbi.range = sizeof(UboData);
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = descriptor_sets_[i];
                w.dstBinding = 0;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.descriptorCount = 1;
                w.pBufferInfo = &dbi;
                vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
            }
        }
    };
}