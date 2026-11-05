#include "vulkan_viewport.h"
#include "theme_manager.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVector2D>
#include <QRect>
#include <algorithm>
#include <cmath>

namespace rvdlrsp::gui
{

    // ─── 工具 ──────────────────────────────────────────────────────
    VkShaderModule ViewportRenderer::load_shader(VkDevice dev, const std::string &path)
    {
        std::ifstream f(path, std::ios::ate | std::ios::binary);
        if (!f)
            throw std::runtime_error("Failed to open shader: " + path);
        size_t size = static_cast<size_t>(f.tellg());
        std::vector<char> buf(size);
        f.seekg(0);
        f.read(buf.data(), static_cast<std::streamsize>(size));

        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = size;
        ci.pCode = reinterpret_cast<const uint32_t *>(buf.data());
        VkShaderModule mod;
        if (vkCreateShaderModule(dev, &ci, nullptr, &mod) != VK_SUCCESS)
            throw std::runtime_error("Failed to create shader module");
        return mod;
    }

    uint32_t ViewportRenderer::find_mem_type(VkPhysicalDevice pd, uint32_t filter,
                                             VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        throw std::runtime_error("No suitable memory type");
    }

    void ViewportRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                         const void *data, VkBuffer &buf, VkDeviceMemory &mem)
    {
        VkDevice dev = window_->device();
        VkPhysicalDevice pd = window_->physicalDevice();

        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS)
            throw std::runtime_error("Failed to create buffer");

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, buf, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem_type(pd, mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate memory");
        vkBindBufferMemory(dev, buf, mem, 0);

        if (data)
        {
            void *mapped;
            vkMapMemory(dev, mem, 0, size, 0, &mapped);
            std::memcpy(mapped, data, static_cast<size_t>(size));
            vkUnmapMemory(dev, mem);
        }
    }

    void ViewportRenderer::destroy_mesh(MeshGpu &m)
    {
        VkDevice dev = window_->device();
        if (m.vb)
            vkDestroyBuffer(dev, m.vb, nullptr);
        if (m.vb_mem)
            vkFreeMemory(dev, m.vb_mem, nullptr);
        if (m.ib)
            vkDestroyBuffer(dev, m.ib, nullptr);
        if (m.ib_mem)
            vkFreeMemory(dev, m.ib_mem, nullptr);
        m = MeshGpu{};
    }

    // ─── 资源 ──────────────────────────────────────────────────────
    void ViewportRenderer::initResources()
    {
        create_pipeline();
        create_uniforms();
        create_sky_pipeline();
        create_sky_uniforms();
        create_line_pipeline();
        create_line_uniforms();
        create_gizmo_uniforms();
        build_grid_axes();
        build_gizmo();
    }

    void ViewportRenderer::releaseResources()
    {
        VkDevice dev = window_->device();
        if (dev == VK_NULL_HANDLE)
            return;
        vkDeviceWaitIdle(dev);
        for (auto &m : gpu_meshes_)
            destroy_mesh(m);
        gpu_meshes_.clear();
        if (uniform_buf_)
            vkDestroyBuffer(dev, uniform_buf_, nullptr);
        if (uniform_mem_)
            vkFreeMemory(dev, uniform_mem_, nullptr);
        if (desc_pool_)
            vkDestroyDescriptorPool(dev, desc_pool_, nullptr);
        if (desc_layout_)
            vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr);
        if (pipeline_)
            vkDestroyPipeline(dev, pipeline_, nullptr);
        if (pipeline_layout_)
            vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
        if (sky_uniform_buf_)
            vkDestroyBuffer(dev, sky_uniform_buf_, nullptr);
        if (sky_uniform_mem_)
            vkFreeMemory(dev, sky_uniform_mem_, nullptr);
        if (sky_desc_pool_)
            vkDestroyDescriptorPool(dev, sky_desc_pool_, nullptr);
        if (sky_desc_layout_)
            vkDestroyDescriptorSetLayout(dev, sky_desc_layout_, nullptr);
        if (sky_pipeline_)
            vkDestroyPipeline(dev, sky_pipeline_, nullptr);
        if (sky_pipeline_layout_)
            vkDestroyPipelineLayout(dev, sky_pipeline_layout_, nullptr);
        if (line_vb_)
            vkDestroyBuffer(dev, line_vb_, nullptr);
        if (line_vb_mem_)
            vkFreeMemory(dev, line_vb_mem_, nullptr);
        if (line_uniform_buf_)
            vkDestroyBuffer(dev, line_uniform_buf_, nullptr);
        if (line_uniform_mem_)
            vkFreeMemory(dev, line_uniform_mem_, nullptr);
        if (line_desc_pool_)
            vkDestroyDescriptorPool(dev, line_desc_pool_, nullptr);
        if (line_desc_layout_)
            vkDestroyDescriptorSetLayout(dev, line_desc_layout_, nullptr);
        if (line_pipeline_)
            vkDestroyPipeline(dev, line_pipeline_, nullptr);
        if (line_pipeline_layout_)
            vkDestroyPipelineLayout(dev, line_pipeline_layout_, nullptr);
        if (gizmo_vb_)
            vkDestroyBuffer(dev, gizmo_vb_, nullptr);
        if (gizmo_vb_mem_)
            vkFreeMemory(dev, gizmo_vb_mem_, nullptr);
        if (gizmo_uniform_buf_)
            vkDestroyBuffer(dev, gizmo_uniform_buf_, nullptr);
        if (gizmo_uniform_mem_)
            vkFreeMemory(dev, gizmo_uniform_mem_, nullptr);
    }

    void ViewportRenderer::upload_meshes()
    {
        VkDevice dev = window_->device();
        vkDeviceWaitIdle(dev);
        for (auto &m : gpu_meshes_)
            destroy_mesh(m);
        gpu_meshes_.resize(meshes_.size());

        for (size_t i = 0; i < meshes_.size(); ++i)
        {
            const render::Mesh &cpu = meshes_[i];
            MeshGpu &gpu = gpu_meshes_[i];

            // render::Vertex 的 position/normal 是 qpc::Vec3（double），
            // 这里转成 float 的 GpuVertex 再上传，避免与管线 R32G32B32 格式错位。
            std::vector<GpuVertex> gpu_verts;
            gpu_verts.reserve(cpu.vertices.size());
            for (const auto &v : cpu.vertices)
            {
                GpuVertex gv{};
                gv.px = static_cast<float>(v.position.x);
                gv.py = static_cast<float>(v.position.y);
                gv.pz = static_cast<float>(v.position.z);
                gv.nx = static_cast<float>(v.normal.x);
                gv.ny = static_cast<float>(v.normal.y);
                gv.nz = static_cast<float>(v.normal.z);
                gv.u = v.u;
                gv.v = v.v;
                gv.r = v.r;
                gv.g = v.g;
                gv.b = v.b;
                gpu_verts.push_back(gv);
            }

            create_buffer(gpu_verts.size() * sizeof(GpuVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gpu_verts.data(),
                          gpu.vb, gpu.vb_mem);
            create_buffer(cpu.indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, cpu.indices.data(),
                          gpu.ib, gpu.ib_mem);
            gpu.index_count = static_cast<uint32_t>(cpu.indices.size());
        }
        rebuild_meshes_ = false;
    }

    void ViewportRenderer::create_pipeline()
    {
        VkDevice dev = window_->device();
        VkShaderModule vert = load_shader(dev, shader_dir_ + "/mesh.vert.spv");
        VkShaderModule frag = load_shader(dev, shader_dir_ + "/mesh.frag.spv");

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GpuVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> attrs(4);
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, nx)};
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,     offsetof(GpuVertex, u)};
        attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, r)};

        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis.vertexBindingDescriptionCount = 1;
        vis.pVertexBindingDescriptions = &binding;
        vis.vertexAttributeDescriptionCount = 4;
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
        rs.cullMode = VK_CULL_MODE_NONE;
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

        VkDescriptorSetLayoutBinding lb{};
        lb.binding = 0;
        lb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings = &lb;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &desc_layout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create descriptor layout");

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(PushBlock);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &desc_layout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(dev, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS)
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
        pi.renderPass = window_->defaultRenderPass();
        pi.subpass = 0;

        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create pipeline");

        vkDestroyShaderModule(dev, vert, nullptr);
        vkDestroyShaderModule(dev, frag, nullptr);
    }

    void ViewportRenderer::create_uniforms()
    {
        VkDevice dev = window_->device();
        create_buffer(sizeof(Ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr,
                      uniform_buf_, uniform_mem_);

        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &desc_pool_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create descriptor pool");

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &desc_layout_;
        if (vkAllocateDescriptorSets(dev, &ai, &desc_set_) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate descriptor set");

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = uniform_buf_;
        dbi.offset = 0;
        dbi.range = sizeof(Ubo);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = desc_set_;
        w.dstBinding = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // ─── 天空管线（全屏三角形 + 大气散射）─────────────────────────
    void ViewportRenderer::create_sky_pipeline()
    {
        VkDevice dev = window_->device();
        VkShaderModule vert = load_shader(dev, shader_dir_ + "/sky.vert.spv");
        VkShaderModule frag = load_shader(dev, shader_dir_ + "/sky.frag.spv");

        // 无顶点输入（用 gl_VertexIndex 生成全屏三角形）
        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis.vertexBindingDescriptionCount = 0;
        vis.vertexAttributeDescriptionCount = 0;

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
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;

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

        VkDescriptorSetLayoutBinding lb{};
        lb.binding = 0;
        lb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings = &lb;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &sky_desc_layout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create sky descriptor layout");

        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &sky_desc_layout_;
        if (vkCreatePipelineLayout(dev, &pl, nullptr, &sky_pipeline_layout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create sky pipeline layout");

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
        pi.layout = sky_pipeline_layout_;
        pi.renderPass = window_->defaultRenderPass();
        pi.subpass = 0;

        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &sky_pipeline_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create sky pipeline");

        vkDestroyShaderModule(dev, vert, nullptr);
        vkDestroyShaderModule(dev, frag, nullptr);
    }

    void ViewportRenderer::create_sky_uniforms()
    {
        VkDevice dev = window_->device();
        create_buffer(sizeof(SkyUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr,
                      sky_uniform_buf_, sky_uniform_mem_);

        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &sky_desc_pool_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create sky descriptor pool");

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = sky_desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &sky_desc_layout_;
        if (vkAllocateDescriptorSets(dev, &ai, &sky_desc_set_) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate sky descriptor set");

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = sky_uniform_buf_;
        dbi.offset = 0;
        dbi.range = sizeof(SkyUbo);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sky_desc_set_;
        w.dstBinding = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // ─── 线框管线（坐标系 + 网格线，LINE_LIST）─────────────────────
    void ViewportRenderer::create_line_pipeline()
    {
        VkDevice dev = window_->device();
        VkShaderModule vert = load_shader(dev, shader_dir_ + "/line.vert.spv");
        VkShaderModule frag = load_shader(dev, shader_dir_ + "/line.frag.spv");

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(LineVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> attrs(2);
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, px)};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, r)};

        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis.vertexBindingDescriptionCount = 1;
        vis.pVertexBindingDescriptions = &binding;
        vis.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
        vis.pVertexAttributeDescriptions = attrs.data();

        VkPipelineInputAssemblyStateCreateInfo ias{};
        ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_LINE;
        rs.cullMode = VK_CULL_MODE_NONE;
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

        VkDescriptorSetLayoutBinding lb{};
        lb.binding = 0;
        lb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings = &lb;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &line_desc_layout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create line descriptor layout");

        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &line_desc_layout_;
        if (vkCreatePipelineLayout(dev, &pl, nullptr, &line_pipeline_layout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create line pipeline layout");

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
        pi.layout = line_pipeline_layout_;
        pi.renderPass = window_->defaultRenderPass();
        pi.subpass = 0;

        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &line_pipeline_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create line pipeline");

        vkDestroyShaderModule(dev, vert, nullptr);
        vkDestroyShaderModule(dev, frag, nullptr);
    }

    void ViewportRenderer::create_line_uniforms()
    {
        VkDevice dev = window_->device();
        create_buffer(sizeof(LineUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr,
                      line_uniform_buf_, line_uniform_mem_);

        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps.descriptorCount = 2;
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 2; // line + gizmo 各一个 descriptor set
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &line_desc_pool_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create line descriptor pool");

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = line_desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &line_desc_layout_;
        if (vkAllocateDescriptorSets(dev, &ai, &line_desc_set_) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate line descriptor set");

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = line_uniform_buf_;
        dbi.offset = 0;
        dbi.range = sizeof(LineUbo);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = line_desc_set_;
        w.dstBinding = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // gizmo 独立 UBO + descriptor set（复用 line_desc_layout_，避免与网格线共享 UBO 覆盖）
    void ViewportRenderer::create_gizmo_uniforms()
    {
        VkDevice dev = window_->device();
        create_buffer(sizeof(LineUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr,
                      gizmo_uniform_buf_, gizmo_uniform_mem_);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = line_desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &line_desc_layout_;
        if (vkAllocateDescriptorSets(dev, &ai, &gizmo_desc_set_) != VK_SUCCESS)
            throw std::runtime_error("Failed to allocate gizmo descriptor set");

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = gizmo_uniform_buf_;
        dbi.offset = 0;
        dbi.range = sizeof(LineUbo);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = gizmo_desc_set_;
        w.dstBinding = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // 生成坐标系（XYZ 轴）与地面网格线（LINE_LIST 顶点）
    void ViewportRenderer::build_grid_axes()
    {
        std::vector<LineVertex> lines;
        auto add = [&](float x1, float y1, float z1, float x2, float y2, float z2,
                       float r, float g, float b)
        {
            lines.push_back({x1, y1, z1, r, g, b});
            lines.push_back({x2, y2, z2, r, g, b});
        };

        // ── 地面网格线（y = -2.0，地面顶面）────────────────────────
        const float grid_y = -2.0f;
        const int half = 10;
        const float step = 1.0f;
        const float ext = half * step;
        for (int i = -half; i <= half; ++i)
        {
            float c = i * step;
            add(-ext, grid_y, c, ext, grid_y, c, 0.28f, 0.30f, 0.34f); // 沿 X 的线
            add(c, grid_y, -ext, c, grid_y, ext, 0.28f, 0.30f, 0.34f); // 沿 Z 的线
        }

        // ── 坐标系（从原点出发，X 红 / Y 绿 / Z 蓝）────────────────
        const float ax = 3.0f;
        add(0, 0, 0, ax, 0, 0, 1.0f, 0.2f, 0.2f); // X
        add(0, 0, 0, 0, ax, 0, 0.2f, 1.0f, 0.2f); // Y
        add(0, 0, 0, 0, 0, ax, 0.2f, 0.4f, 1.0f); // Z

        line_vertex_count_ = static_cast<uint32_t>(lines.size());

        VkDevice dev = window_->device();
        if (line_vb_)
            vkDestroyBuffer(dev, line_vb_, nullptr);
        if (line_vb_mem_)
            vkFreeMemory(dev, line_vb_mem_, nullptr);
        create_buffer(lines.size() * sizeof(LineVertex),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, lines.data(),
                      line_vb_, line_vb_mem_);
    }

    // 构建视口导航 gizmo 顶点（XYZ 三轴 + 立方体框，LINE_LIST）
    void ViewportRenderer::build_gizmo()
    {
        std::vector<LineVertex> lines;
        auto add = [&](float x1, float y1, float z1, float x2, float y2, float z2,
                       float r, float g, float b)
        {
            lines.push_back({x1, y1, z1, r, g, b});
            lines.push_back({x2, y2, z2, r, g, b});
        };

        // ── 三轴（X 红 / Y 绿 / Z 蓝，稍长于立方体）────────────
        const float ax = 1.3f;
        add(0, 0, 0, ax, 0, 0, 1.0f, 0.25f, 0.25f); // X
        add(0, 0, 0, 0, ax, 0, 0.25f, 1.0f, 0.25f); // Y
        add(0, 0, 0, 0, 0, ax, 0.25f, 0.4f, 1.0f);  // Z

        // ── 立方体 12 条边（半长 h，灰色）──────────────────────
        const float h = 0.9f;
        const float gr = 0.55f, gg = 0.58f, gb = 0.62f;
        // 底面 4 条
        add(-h, -h, -h,  h, -h, -h, gr, gg, gb);
        add( h, -h, -h,  h, -h,  h, gr, gg, gb);
        add( h, -h,  h, -h, -h,  h, gr, gg, gb);
        add(-h, -h,  h, -h, -h, -h, gr, gg, gb);
        // 顶面 4 条
        add(-h,  h, -h,  h,  h, -h, gr, gg, gb);
        add( h,  h, -h,  h,  h,  h, gr, gg, gb);
        add( h,  h,  h, -h,  h,  h, gr, gg, gb);
        add(-h,  h,  h, -h,  h, -h, gr, gg, gb);
        // 竖直 4 条
        add(-h, -h, -h, -h,  h, -h, gr, gg, gb);
        add( h, -h, -h,  h,  h, -h, gr, gg, gb);
        add( h, -h,  h,  h,  h,  h, gr, gg, gb);
        add(-h, -h,  h, -h,  h,  h, gr, gg, gb);

        gizmo_vertex_count_ = static_cast<uint32_t>(lines.size());

        VkDevice dev = window_->device();
        if (gizmo_vb_)
            vkDestroyBuffer(dev, gizmo_vb_, nullptr);
        if (gizmo_vb_mem_)
            vkFreeMemory(dev, gizmo_vb_mem_, nullptr);
        create_buffer(lines.size() * sizeof(LineVertex),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, lines.data(),
                      gizmo_vb_, gizmo_vb_mem_);
    }

    // 渲染 gizmo 到右上角小视口（正交投影 + 相机旋转）
    void ViewportRenderer::render_gizmo(VkCommandBuffer cmd, int w, int h)
    {
        if (gizmo_vb_ == VK_NULL_HANDLE || gizmo_vertex_count_ == 0)
            return;

        // gizmo 屏幕区域（右上角）
        const int margin = 12;
        const int size = 120;
        int gx = w - size - margin;
        int gy = margin;

        // 相机旋转矩阵（lookAt 去掉平移），让 gizmo 跟随视角朝向
        float cp = std::cos(state_.camera_pitch);
        float sp = std::sin(state_.camera_pitch);
        float cy = std::cos(state_.camera_yaw);
        float sy = std::sin(state_.camera_yaw);
        QVector3D center(state_.camera_target[0], state_.camera_target[1], state_.camera_target[2]);
        QVector3D eye(center.x() + state_.camera_dist * cp * sy,
                      center.y() + state_.camera_dist * sp,
                      center.z() + state_.camera_dist * cp * cy);
        QMatrix4x4 view;
        view.lookAt(eye, center, QVector3D(0, 1, 0));
        view.setColumn(3, QVector4D(0, 0, 0, 1)); // 去掉平移，仅保留旋转

        QMatrix4x4 ortho;
        ortho.ortho(-1.6f, 1.6f, -1.6f, 1.6f, -2.0f, 2.0f);
        QMatrix4x4 mvp = ortho * view;

        LineUbo lu{};
        std::memcpy(lu.mvp, mvp.constData(), sizeof(float) * 16);
        lu.tint[0] = 1.0f; // gizmo 颜色不随主题变化（保持红绿蓝轴）
        lu.tint[1] = 1.0f;
        lu.tint[2] = 1.0f;
        lu.tint[3] = 0.0f;
        void *mapped;
        VkDevice dev = window_->device();
        vkMapMemory(dev, gizmo_uniform_mem_, 0, sizeof(LineUbo), 0, &mapped);
        std::memcpy(mapped, &lu, sizeof(LineUbo));
        vkUnmapMemory(dev, gizmo_uniform_mem_);

        // 切换到 gizmo 小视口
        VkViewport vp{};
        vp.x = static_cast<float>(gx);
        vp.y = static_cast<float>(gy);
        vp.width = static_cast<float>(size);
        vp.height = static_cast<float>(size);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.offset = {gx, gy};
        sc.extent = {static_cast<uint32_t>(size), static_cast<uint32_t>(size)};
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline_layout_,
                                0, 1, &gizmo_desc_set_, 0, nullptr);
        VkDeviceSize loff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gizmo_vb_, &loff);
        vkCmdDraw(cmd, gizmo_vertex_count_, 1, 0, 0);
    }

    // ─── 每帧渲染 ──────────────────────────────────────────────────
    void ViewportRenderer::startNextFrame()
    {
        VkDevice dev = window_->device();
        VkCommandBuffer cmd = window_->currentCommandBuffer();
        QSize sz = window_->swapChainImageSize();

        if (rebuild_meshes_ && !meshes_.empty())
            upload_meshes();

        // 相机（环绕中心取自 state_，支持平移）
        QVector3D center(state_.camera_target[0],
                         state_.camera_target[1],
                         state_.camera_target[2]);
        float cp = std::cos(state_.camera_pitch);
        float sp = std::sin(state_.camera_pitch);
        float cy = std::cos(state_.camera_yaw);
        float sy = std::sin(state_.camera_yaw);
        QVector3D eye(center.x() + state_.camera_dist * cp * sy,
                      center.y() + state_.camera_dist * sp,
                      center.z() + state_.camera_dist * cp * cy);
        QMatrix4x4 proj;
        float aspect = float(sz.width()) / float(sz.height());
        if (state_.ortho)
        {
            // 正交投影：半高与透视在 target 处的可见高度一致
            float half_h = state_.camera_dist * std::tan(30.0f * 3.14159265358979f / 180.0f);
            proj.ortho(-half_h * aspect, half_h * aspect, -half_h, half_h, 0.1f, 100.0f);
        }
        else
        {
            proj.perspective(60.0f, aspect, 0.1f, 100.0f);
        }
        // Vulkan 的 clip space Y 轴向下，而 QMatrix4x4 按 OpenGL 约定（Y 向上）生成投影矩阵，
        // 必须预乘 clipCorrectionMatrix 修正 Y 翻转与深度范围，否则画面会上下颠倒。
        proj = window_->clipCorrectionMatrix() * proj;
        QMatrix4x4 view;
        view.lookAt(eye, center, QVector3D(0, 1, 0));

        // 视口 / 裁剪（天空 + 网格共用）
        VkViewport vp{};
        vp.width = static_cast<float>(sz.width());
        vp.height = static_cast<float>(sz.height());
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = {static_cast<uint32_t>(sz.width()), static_cast<uint32_t>(sz.height())};
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // ── 开始 render pass（QVulkanWindow 不会自动 begin，必须手动）──
        VkClearValue clears[2]{};
        // 背景色 / 太阳强度 / 模型光照随白天/夜间主题联动（纯 C++，无需改 shader）
        const ThemeManager::ViewportTheme vt = ThemeManager::viewportTheme(state_.dark_mode);
        clears[0].color = {{vt.clear_r, vt.clear_g, vt.clear_b, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = window_->defaultRenderPass();
        rp.framebuffer = window_->currentFramebuffer();
        rp.renderArea.extent = {static_cast<uint32_t>(sz.width()),
                                static_cast<uint32_t>(sz.height())};
        rp.clearValueCount = 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        // ── 天空 pass（全屏三角形 + 大气散射，先画）────────────────
        if (sky_pipeline_ != VK_NULL_HANDLE)
        {
            QMatrix4x4 inv_view_proj = (proj * view).inverted();
            SkyUbo su{};
            std::memcpy(su.inv_view_proj, inv_view_proj.constData(), sizeof(float) * 16);
            su.sun_dir[0] = state_.sun_dir[0];
            su.sun_dir[1] = state_.sun_dir[1];
            su.sun_dir[2] = state_.sun_dir[2];
            su.sun_dir[3] = vt.sun_intensity; // 太阳强度
            su.cam_pos[0] = eye.x();
            su.cam_pos[1] = eye.y();
            su.cam_pos[2] = eye.z();
            su.cam_pos[3] = state_.dark_mode ? 1.0f : 0.0f; // 白天/夜间

            void *smapped;
            vkMapMemory(dev, sky_uniform_mem_, 0, sizeof(SkyUbo), 0, &smapped);
            std::memcpy(smapped, &su, sizeof(SkyUbo));
            vkUnmapMemory(dev, sky_uniform_mem_);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_layout_,
                                    0, 1, &sky_desc_set_, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0); // 全屏三角形
        }

        // ── 线框 pass（坐标系 + 网格线）────────────────────────────
        if (line_pipeline_ != VK_NULL_HANDLE && line_vertex_count_ > 0)
        {
            QMatrix4x4 line_mvp = proj * view; // 顶点已是世界坐标，模型 = 单位矩阵
            LineUbo lu{};
            std::memcpy(lu.mvp, line_mvp.constData(), sizeof(float) * 16);
            lu.tint[0] = vt.grid_tint_r; // 网格线颜色随白天/夜间主题切换
            lu.tint[1] = vt.grid_tint_g;
            lu.tint[2] = vt.grid_tint_b;
            lu.tint[3] = 0.0f;

            void *lmapped;
            vkMapMemory(dev, line_uniform_mem_, 0, sizeof(LineUbo), 0, &lmapped);
            std::memcpy(lmapped, &lu, sizeof(LineUbo));
            vkUnmapMemory(dev, line_uniform_mem_);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline_layout_,
                                    0, 1, &line_desc_set_, 0, nullptr);
            VkDeviceSize loff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &line_vb_, &loff);
            vkCmdDraw(cmd, line_vertex_count_, 1, 0, 0);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &desc_set_, 0, nullptr);

        float light_dir[3] = {0.3f, -1.0f, 0.5f};
        float cam_pos[3] = {eye.x(), eye.y(), eye.z()};

        // 光照/材质等共享参数写入一次 UBO（所有实例共用）
        Ubo ubo{};
        ubo.light_dir[0] = light_dir[0];
        ubo.light_dir[1] = light_dir[1];
        ubo.light_dir[2] = light_dir[2];
        ubo.light_intensity = vt.light_intensity;
        ubo.cam_pos[0] = cam_pos[0];
        ubo.cam_pos[1] = cam_pos[1];
        ubo.cam_pos[2] = cam_pos[2];
        ubo.dark_mode = state_.dark_mode ? 1.0f : 0.0f; // 白天/夜间
        ubo.base_color[0] = 0.8f;
        ubo.base_color[1] = 0.8f;
        ubo.base_color[2] = 0.8f;
        ubo.metallic = 0.0f;
        ubo.roughness = 0.5f;
        ubo.use_texture = 0.0f;
        {
            void *mapped;
            vkMapMemory(dev, uniform_mem_, 0, sizeof(Ubo), 0, &mapped);
            std::memcpy(mapped, &ubo, sizeof(Ubo));
            vkUnmapMemory(dev, uniform_mem_);
        }

        // 每个实例用 push constant 传递独立的 mvp/model，避免 UBO 覆盖
        for (const auto &inst : state_.instances)
        {
            if (inst.mesh_id < 0 || inst.mesh_id >= static_cast<int>(gpu_meshes_.size()))
                continue;

            QMatrix4x4 model;
            model.translate(inst.position);
            model.rotate(inst.rotation);
            model.scale(inst.scale);
            QMatrix4x4 mvp = proj * view * model;

            PushBlock pc{};
            std::memcpy(pc.mvp, mvp.constData(), sizeof(float) * 16);
            std::memcpy(pc.model, model.constData(), sizeof(float) * 16);
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(PushBlock), &pc);

            const MeshGpu &m = gpu_meshes_[inst.mesh_id];
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vb, &offset);
            vkCmdBindIndexBuffer(cmd, m.ib, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m.index_count, 1, 0, 0, 0);
        }

        // ── 视口导航 gizmo（右上角，最后绘制）──────────────────────
        render_gizmo(cmd, sz.width(), sz.height());

        vkCmdEndRenderPass(cmd);
        window_->frameReady();
        // QVulkanWindow 非默认持续渲染：必须显式请求下一帧，否则画面冻结在首帧，
        // 相机交互/实例动画都不会刷新。
        window_->requestUpdate();
    }

    // ─── QVulkanViewport ───────────────────────────────────────────
    QVulkanViewport::QVulkanViewport(const std::string &shader_dir)
        : shader_dir_(shader_dir)
    {
        setTitle(QStringLiteral("3D 仿真视口"));
        // WASD 移动：定时器每 16ms 采样一次按键状态
        connect(&wsad_timer_, &QTimer::timeout, this, &QVulkanViewport::move_camera_wsad);
        wsad_timer_.start(16);
    }

    void QVulkanViewport::set_state(const ViewportState &s)
    {
        state_ = s;
        sync_state_to_renderer();
    }

    void QVulkanViewport::set_meshes(const std::vector<render::Mesh> &meshes)
    {
        meshes_ = meshes;
        if (renderer_)
            renderer_->set_meshes(meshes);
    }

    void QVulkanViewport::set_scene(const std::vector<InstanceTransform> &instances,
                                    const float sun_dir[3])
    {
        // 只更新场景实例与太阳，保留相机交互状态
        state_.instances = instances;
        if (sun_dir)
        {
            state_.sun_dir[0] = sun_dir[0];
            state_.sun_dir[1] = sun_dir[1];
            state_.sun_dir[2] = sun_dir[2];
        }
        sync_state_to_renderer();
    }

    void QVulkanViewport::set_dark_mode(bool dark)
    {
        state_.dark_mode = dark;
        sync_state_to_renderer();
    }

    void QVulkanViewport::reset_camera()
    {
        state_.camera_yaw = -0.8f;
        state_.camera_pitch = 0.45f;
        state_.camera_dist = 14.0f;
        state_.camera_target[0] = 0.0f;
        state_.camera_target[1] = 1.0f;
        state_.camera_target[2] = 0.0f;
        state_.ortho = false;
        sync_state_to_renderer();
    }

    void QVulkanViewport::sync_state_to_renderer()
    {
        if (renderer_)
            renderer_->set_state(state_);
    }

    QVulkanWindowRenderer *QVulkanViewport::createRenderer()
    {
        renderer_ = new ViewportRenderer(this, shader_dir_);
        renderer_->set_state(state_);
        if (!meshes_.empty())
            renderer_->set_meshes(meshes_);
        return renderer_;
    }

    void QVulkanViewport::mousePressEvent(QMouseEvent *event)
    {
        if (event->button() == Qt::RightButton)  { orbiting_ = true; last_pos_ = event->position().toPoint(); event->accept(); return; }
        if (event->button() == Qt::MiddleButton) { panning_   = true; last_pos_ = event->position().toPoint(); event->accept(); return; }
        if (event->button() == Qt::LeftButton)
        {
            // 先检测是否点击到 gizmo（切换视图），否则按旋转实体处理
            if (handle_gizmo_click(event->position().toPoint()))
            {
                event->accept();
                return;
            }
            rotating_entity_ = true;
            last_pos_ = event->position().toPoint();
            event->accept();
            return;
        }
        QVulkanWindow::mousePressEvent(event);
    }

    void QVulkanViewport::mouseMoveEvent(QMouseEvent *event)
    {
        QPoint cur = event->position().toPoint();
        float dx = static_cast<float>(cur.x() - last_pos_.x());
        float dy = static_cast<float>(cur.y() - last_pos_.y());
        last_pos_ = cur;

        if (orbiting_)        { orbit_camera(dx, dy); event->accept(); return; }
        if (panning_)         { pan_camera(dx, dy);   event->accept(); return; }
        if (rotating_entity_) { emit dragDelta(dx, dy); event->accept(); return; }
        QVulkanWindow::mouseMoveEvent(event);
    }

    void QVulkanViewport::mouseReleaseEvent(QMouseEvent *event)
    {
        if (event->button() == Qt::RightButton)  { orbiting_ = false; event->accept(); return; }
        if (event->button() == Qt::MiddleButton) { panning_ = false;  event->accept(); return; }
        if (event->button() == Qt::LeftButton)   { rotating_entity_ = false; event->accept(); return; }
        QVulkanWindow::mouseReleaseEvent(event);
    }

    void QVulkanViewport::wheelEvent(QWheelEvent *event)
    {
        float steps = event->angleDelta().y() / 120.0f;
        dolly_camera(steps);
        event->accept();
    }

    void QVulkanViewport::orbit_camera(float dx, float dy)
    {
        state_.camera_yaw   -= dx * 0.008f;
        state_.camera_pitch += dy * 0.008f;
        const float lim = 1.55f; // 约 89°，避免越过极点翻转（UE5 行为）
        state_.camera_pitch = std::clamp(state_.camera_pitch, -lim, lim);
        sync_state_to_renderer();
    }

    void QVulkanViewport::pan_camera(float dx, float dy)
    {
        float cp = std::cos(state_.camera_pitch);
        float sp = std::sin(state_.camera_pitch);
        float cy = std::cos(state_.camera_yaw);
        float sy = std::sin(state_.camera_yaw);

        // 相机前向（看向 center）
        QVector3D fwd(-cp * sy, -sp, -cp * cy);
        QVector3D right = QVector3D::crossProduct(fwd, QVector3D(0.0f, 1.0f, 0.0f));
        if (right.lengthSquared() < 1e-6f)
            right = QVector3D(1.0f, 0.0f, 0.0f);
        right.normalize();
        QVector3D up = QVector3D::crossProduct(right, fwd).normalized();

        float scale = state_.camera_dist * 0.0015f;
        // 拖拽右移 → 场景右移 → 相机反向移动
        state_.camera_target[0] += (-right.x() * dx + up.x() * dy) * scale;
        state_.camera_target[1] += (-right.y() * dx + up.y() * dy) * scale;
        state_.camera_target[2] += (-right.z() * dx + up.z() * dy) * scale;
        sync_state_to_renderer();
    }

    void QVulkanViewport::dolly_camera(float steps)
    {
        // 指数缩放，滚轮向上（steps>0）靠近
        state_.camera_dist *= std::pow(0.9f, steps);
        state_.camera_dist = std::clamp(state_.camera_dist, 0.5f, 200.0f);
        sync_state_to_renderer();
    }

    // 检测 gizmo 点击：命中则切换到对应正交视图，返回 true
    bool QVulkanViewport::handle_gizmo_click(const QPoint &pos)
    {
        int w = width();
        int h = height();
        int gx = w - GIZMO_SIZE - GIZMO_MARGIN;
        int gy = GIZMO_MARGIN;
        QRect rect(gx, gy, GIZMO_SIZE, GIZMO_SIZE);
        if (!rect.contains(pos))
            return false;

        // 点击相对 gizmo 中心的偏移（屏幕坐标，y 向下）
        float cx = gx + GIZMO_SIZE * 0.5f;
        float cy = gy + GIZMO_SIZE * 0.5f;
        QVector2D click(pos.x() - cx, pos.y() - cy);

        // 相机旋转矩阵（与 render_gizmo 一致，去掉平移）
        float cp = std::cos(state_.camera_pitch);
        float sp = std::sin(state_.camera_pitch);
        float cyaw = std::cos(state_.camera_yaw);
        float syaw = std::sin(state_.camera_yaw);
        QVector3D center(state_.camera_target[0], state_.camera_target[1], state_.camera_target[2]);
        QVector3D eye(center.x() + state_.camera_dist * cp * syaw,
                      center.y() + state_.camera_dist * sp,
                      center.z() + state_.camera_dist * cp * cyaw);
        QMatrix4x4 view;
        view.lookAt(eye, center, QVector3D(0, 1, 0));
        view.setColumn(3, QVector4D(0, 0, 0, 1));

        // 三个世界轴在屏幕 2D 的投影（y 翻转，屏幕 y 向下）
        QVector3D ax = view.mapVector(QVector3D(1, 0, 0));
        QVector3D ay = view.mapVector(QVector3D(0, 1, 0));
        QVector3D az = view.mapVector(QVector3D(0, 0, 1));
        QVector2D sx(ax.x(), -ax.y());
        QVector2D syv(ay.x(), -ay.y());
        QVector2D sz(az.x(), -az.y());

        // 点击向量与三个轴屏幕方向点积，绝对值最大者即命中轴
        float dx_ = QVector2D::dotProduct(click.normalized(), sx.normalized());
        float dy_ = QVector2D::dotProduct(click.normalized(), syv.normalized());
        float dz_ = QVector2D::dotProduct(click.normalized(), sz.normalized());

        float ax_ = std::abs(dx_);
        float ay_ = std::abs(dy_);
        float az_ = std::abs(dz_);

        // 距离 gizmo 中心太近视为回到透视视图
        float dist2 = click.lengthSquared();
        if (dist2 < (GIZMO_SIZE * 0.18f) * (GIZMO_SIZE * 0.18f))
        {
            state_.ortho = false;
            sync_state_to_renderer();
            return true;
        }

        if (ax_ >= ay_ && ax_ >= az_)
            set_view_axis(QVector3D(dx_ >= 0 ? 1.0f : -1.0f, 0, 0));
        else if (ay_ >= az_)
            set_view_axis(QVector3D(0, dy_ >= 0 ? 1.0f : -1.0f, 0));
        else
            set_view_axis(QVector3D(0, 0, dz_ >= 0 ? 1.0f : -1.0f));
        return true;
    }

    // 切换到沿给定世界轴方向观察的正交视图
    void QVulkanViewport::set_view_axis(const QVector3D &dir)
    {
        // 由方向向量反推 yaw/pitch：eye - center = dist * (cp*sy, sp, cp*cy)
        // 目标方向 d（归一化）
        QVector3D d = dir.normalized();
        float pitch = std::asin(std::clamp(d.y(), -1.0f, 1.0f));
        float cp = std::cos(pitch);
        float yaw;
        if (std::abs(cp) < 1e-4f)
        {
            // 俯仰接近 ±90°，yaw 保持当前值即可
            yaw = state_.camera_yaw;
        }
        else
        {
            yaw = std::atan2(d.x() / cp, d.z() / cp);
        }
        state_.camera_pitch = pitch;
        state_.camera_yaw = yaw;
        state_.ortho = true;
        sync_state_to_renderer();
    }

    void QVulkanViewport::keyPressEvent(QKeyEvent *event)
    {
        switch (event->key())
        {
        case Qt::Key_W: key_w_ = true; event->accept(); return;
        case Qt::Key_S: key_s_ = true; event->accept(); return;
        case Qt::Key_A: key_a_ = true; event->accept(); return;
        case Qt::Key_D: key_d_ = true; event->accept(); return;
        }
        QVulkanWindow::keyPressEvent(event);
    }

    void QVulkanViewport::keyReleaseEvent(QKeyEvent *event)
    {
        switch (event->key())
        {
        case Qt::Key_W: key_w_ = false; event->accept(); return;
        case Qt::Key_S: key_s_ = false; event->accept(); return;
        case Qt::Key_A: key_a_ = false; event->accept(); return;
        case Qt::Key_D: key_d_ = false; event->accept(); return;
        }
        QVulkanWindow::keyReleaseEvent(event);
    }

    // WASD：相机前后左右移动（沿水平面，不改变高度）
    void QVulkanViewport::move_camera_wsad()
    {
        if (!(key_w_ || key_s_ || key_a_ || key_d_))
            return;

        // 相机前向的水平分量（忽略 pitch 垂直分量，保持水平移动）
        float cy = std::cos(state_.camera_yaw);
        float sy = std::sin(state_.camera_yaw);
        QVector3D fwd(-sy, 0.0f, -cy);   // 看向 center 的水平方向
        QVector3D right(-cy, 0.0f, sy);  // 右向量

        QVector3D move(0.0f, 0.0f, 0.0f);
        if (key_w_) move += fwd;
        if (key_s_) move -= fwd;
        if (key_d_) move += right;
        if (key_a_) move -= right;

        if (move.lengthSquared() < 1e-6f)
            return;

        float speed = state_.camera_dist * 0.5f; // 速度随距离缩放，保持手感一致
        QVector3D delta = move.normalized() * speed * 0.016f; // 每 tick 16ms

        state_.camera_target[0] += delta.x();
        state_.camera_target[1] += delta.y();
        state_.camera_target[2] += delta.z();
        sync_state_to_renderer();
    }

    void QVulkanViewport::get_camera_vectors(QVector3D &fwd, QVector3D &right, QVector3D &up) const
    {
        float cp = std::cos(state_.camera_pitch);
        float sp = std::sin(state_.camera_pitch);
        float cy = std::cos(state_.camera_yaw);
        float sy = std::sin(state_.camera_yaw);

        // 相机前向（看向 center）
        fwd = QVector3D(-cp * sy, -sp, -cp * cy);
        right = QVector3D::crossProduct(fwd, QVector3D(0.0f, 1.0f, 0.0f));
        if (right.lengthSquared() < 1e-6f)
            right = QVector3D(1.0f, 0.0f, 0.0f);
        right.normalize();
        up = QVector3D::crossProduct(right, fwd).normalized();
    }
}