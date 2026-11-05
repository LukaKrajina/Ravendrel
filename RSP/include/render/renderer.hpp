#pragma once
#include <string>
#include <memory>
#include <array>
#include <vector>
#include <iostream>
#include "scene.hpp"
#include "mat4.hpp"
#include "vulkan_context.hpp"
#include "mesh.hpp"
#include "texture.hpp"
#include "texture_decoder.hpp"
#include "pipeline3d.hpp"

namespace rvdlrsp::render
{

    class Renderer
    {
    private:
        VulkanContext ctx_;
        Pipeline3D pipeline_;
        std::vector<GpuMesh> meshes_;
        std::vector<VulkanTexture> material_textures_; // material_id → GPU 纹理
        VulkanTexture default_texture_;                // 1x1 白色默认纹理
        Scene scene_;
        bool initialized_ = false;
        bool default_texture_ready_ = false;

    public:
        bool init(const std::string &title, int w, int h,
                  const std::string &vert_spv, const std::string &frag_spv)
        {
            if (!ctx_.init(title.c_str(), w, h))
                return false;
            pipeline_.init(ctx_, vert_spv, frag_spv);
            create_default_texture();
            initialized_ = true;
            return true;
        }

        void shutdown()
        {
            material_textures_.clear();
            default_texture_.destroy();
            initialized_ = false;
        }

        // 提交场景
        // 网格仅在数量变化时重新上传；材质纹理解码并上传
        void submit_scene(const Scene &scene)
        {
            bool need_upload = (meshes_.size() != scene.meshes.size());
            scene_ = scene;
            if (need_upload)
            {
                meshes_.clear();
                meshes_.resize(scene_.meshes.size());
                for (size_t i = 0; i < scene_.meshes.size(); ++i)
                    meshes_[i].upload(ctx_, scene_.meshes[i]);
            }
            upload_material_textures();
        }

        bool should_close() const { return ctx_.should_close(); }
        bool initialized() const { return initialized_; }

        // 渲染一帧
        void render_frame()
        {
            if (!initialized_)
                return;
            ctx_.poll_events();

            uint32_t image_index = 0;
            VkCommandBuffer cmd = ctx_.begin_frame(image_index);
            if (cmd == VK_NULL_HANDLE)
                return;

            const Camera &cam = scene_.camera;
            double aspect = static_cast<double>(ctx_.extent().width) / ctx_.extent().height;
            Mat4 proj = Mat4::perspective(cam.fov_deg, aspect, cam.near_plane, cam.far_plane);
            Mat4 view = Mat4::look_at(cam.position, cam.target, {0, 1, 0});

            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = ctx_.render_pass();
            rp.framebuffer = framebuffer(image_index);
            rp.renderArea.extent = ctx_.extent();
            std::array<VkClearValue, 2> clears{};
            clears[0].color = {{0.1f, 0.12f, 0.15f, 1.0f}};
            clears[1].depthStencil = {1.0f, 0};
            rp.clearValueCount = static_cast<uint32_t>(clears.size());
            rp.pClearValues = clears.data();

            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            pipeline_.bind(cmd, image_index); // 绑定 pipeline + viewport + scissor

            float light_dir[3] = {
                static_cast<float>(scene_.light.direction.x),
                static_cast<float>(scene_.light.direction.y),
                static_cast<float>(scene_.light.direction.z)};
            float cam_pos[3] = {
                static_cast<float>(cam.position.x),
                static_cast<float>(cam.position.y),
                static_cast<float>(cam.position.z)};

            // 绘制每个实例（绑定 PBR 材质 + baseColor 纹理）
            for (const auto &inst : scene_.instances)
            {
                if (inst.mesh_id >= meshes_.size())
                    continue;

                Material mat;
                if (inst.material_id < scene_.materials.size())
                    mat = scene_.materials[inst.material_id];

                Mat4 model = Mat4::model(inst.position, inst.orientation, inst.scale);
                Mat4 mvp = proj * view * model;
                pipeline_.update_uniform(image_index, mvp, model, light_dir,
                                         scene_.light.intensity, cam_pos, mat);

                // 绑定纹理（有材质纹理用实际纹理，否则默认纹理）
                VulkanTexture *tex = &default_texture_;
                if (inst.material_id < material_textures_.size() &&
                    material_textures_[inst.material_id].valid())
                    tex = &material_textures_[inst.material_id];
                pipeline_.bind_texture(image_index, tex->sampler(), tex->view());
                pipeline_.bind_descriptor(cmd, image_index);

                VkDeviceSize offset = 0;
                VkBuffer vb = meshes_[inst.mesh_id].vertex_buffer();
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                vkCmdBindIndexBuffer(cmd, meshes_[inst.mesh_id].index_buffer(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, meshes_[inst.mesh_id].index_count(), 1, 0, 0, 0);
            }

            vkCmdEndRenderPass(cmd);
            ctx_.end_frame(image_index, cmd);
        }

    private:
        VkFramebuffer framebuffer(uint32_t index) { return ctx_.framebuffer(index); }

        // 创建 1x1 默认纹理(白色)
        void create_default_texture()
        {
            DecodedImage img;
            img.width = 1;
            img.height = 1;
            img.pixels = {255, 255, 255, 255};
            img.valid = true;
            default_texture_.upload(ctx_, img);
            default_texture_ready_ = default_texture_.valid();
        }

        // 解码并上传所有材质纹理
        void upload_material_textures()
        {
            material_textures_.clear();
            material_textures_.resize(scene_.materials.size());
            for (size_t i = 0; i < scene_.materials.size(); ++i)
            {
                const Material &mat = scene_.materials[i];
                if (!mat.base_color_texture.valid)
                    continue;
                DecodedImage img = TextureDecoder::decode(mat.base_color_texture);
                if (!img.valid)
                    continue;
                material_textures_[i].upload(ctx_, img);
            }
        }
    };
}