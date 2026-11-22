#pragma once
#include <vulkan/vulkan.h>
#include <QVulkanWindow>
#include <QVulkanInstance>
#include <QVector3D>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QTimer>
#include <QKeyEvent>
#include <vector>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "render/scene.hpp"

namespace rvdlrsp::gui
{

    struct InstanceTransform
    {
        int mesh_id = 0;
        QVector3D position;
        QQuaternion rotation;
        QVector3D scale{1, 1, 1};
    };

    struct ViewportState
    {
        float camera_yaw = -0.8f;
        float camera_pitch = 0.45f;
        float camera_dist = 14.0f;
        float camera_target[3] = {0.0f, 1.0f, 0.0f};
        float sun_dir[3] = {0.35f, 0.75f, 0.45f};
        bool ortho = false;
        bool dark_mode = true;
        std::vector<InstanceTransform> instances;
    };

    class ViewportRenderer : public QVulkanWindowRenderer
    {
    public:
        ViewportRenderer(QVulkanWindow *window, const std::string &shader_dir)
            : window_(window), shader_dir_(shader_dir) {}

        void set_state(const ViewportState &s) { state_ = s; }
        void set_meshes(const std::vector<render::Mesh> &meshes)
        {
            meshes_ = meshes;
            rebuild_meshes_ = true;
        }

        void startNextFrame() override;
        void initResources() override;
        void releaseResources() override;

    private:
        void create_pipeline();
        void create_uniforms();
        void create_sky_pipeline();
        void create_sky_uniforms();
        void create_line_pipeline();
        void create_line_uniforms();
        void create_gizmo_uniforms();
        void build_grid_axes();
        void build_gizmo();
        void render_gizmo(VkCommandBuffer cmd, int w, int h);
        void upload_meshes();

        struct MeshGpu
        {
            VkBuffer vb = VK_NULL_HANDLE;
            VkDeviceMemory vb_mem = VK_NULL_HANDLE;
            VkBuffer ib = VK_NULL_HANDLE;
            VkDeviceMemory ib_mem = VK_NULL_HANDLE;
            uint32_t index_count = 0;
        };

        struct GpuVertex
        {
            float px, py, pz;
            float nx, ny, nz;
            float u, v;
            float r, g, b;
        };

        struct alignas(16) Ubo
        {
            float light_dir[3];
            float light_intensity;
            float cam_pos[3];
            float dark_mode;
            float base_color[3];
            float metallic;
            float roughness;
            float use_texture;
        };

        struct PushBlock
        {
            float mvp[16];
            float model[16];
        };

        struct alignas(16) SkyUbo
        {
            float inv_view_proj[16];
            float sun_dir[4];
            float cam_pos[4];
        };

        struct LineVertex
        {
            float px, py, pz;
            float r, g, b;
        };

        struct alignas(16) LineUbo
        {
            float mvp[16];
            float tint[4];
        };

        QVulkanWindow *window_;
        std::string shader_dir_;
        ViewportState state_;

        VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
        VkBuffer uniform_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory uniform_mem_ = VK_NULL_HANDLE;

        VkPipelineLayout sky_pipeline_layout_ = VK_NULL_HANDLE;
        VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout sky_desc_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool sky_desc_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet sky_desc_set_ = VK_NULL_HANDLE;
        VkBuffer sky_uniform_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory sky_uniform_mem_ = VK_NULL_HANDLE;

        VkPipelineLayout line_pipeline_layout_ = VK_NULL_HANDLE;
        VkPipeline line_pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout line_desc_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool line_desc_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet line_desc_set_ = VK_NULL_HANDLE;
        VkBuffer line_uniform_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory line_uniform_mem_ = VK_NULL_HANDLE;
        VkBuffer line_vb_ = VK_NULL_HANDLE;
        VkDeviceMemory line_vb_mem_ = VK_NULL_HANDLE;
        uint32_t line_vertex_count_ = 0;

        VkBuffer gizmo_vb_ = VK_NULL_HANDLE;
        VkDeviceMemory gizmo_vb_mem_ = VK_NULL_HANDLE;
        uint32_t gizmo_vertex_count_ = 0;
        VkBuffer gizmo_uniform_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory gizmo_uniform_mem_ = VK_NULL_HANDLE;
        VkDescriptorSet gizmo_desc_set_ = VK_NULL_HANDLE;

        std::vector<render::Mesh> meshes_;
        std::vector<MeshGpu> gpu_meshes_;
        bool rebuild_meshes_ = false;

        static VkShaderModule load_shader(VkDevice dev, const std::string &path);
        static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t filter, VkMemoryPropertyFlags props);
        void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data,
                           VkBuffer &buf, VkDeviceMemory &mem);
        void destroy_mesh(MeshGpu &m);
    };

    class QVulkanViewport : public QVulkanWindow
    {
        Q_OBJECT
    public:
        QVulkanViewport(const std::string &shader_dir);
        void set_state(const ViewportState &s);
        void set_meshes(const std::vector<render::Mesh> &meshes);
        void set_scene(const std::vector<InstanceTransform> &instances,
                       const float sun_dir[3]);
        void set_dark_mode(bool dark);
        void reset_camera();
        QVulkanWindowRenderer *createRenderer() override;
        void get_camera_vectors(QVector3D &fwd, QVector3D &right, QVector3D &up) const;

    signals:
        void dragDelta(float dx, float dy);

    protected:
        void mousePressEvent(QMouseEvent *event) override;
        void mouseMoveEvent(QMouseEvent *event) override;
        void mouseReleaseEvent(QMouseEvent *event) override;
        void wheelEvent(QWheelEvent *event) override;
        void keyPressEvent(QKeyEvent *event) override;
        void keyReleaseEvent(QKeyEvent *event) override;

    private:
        void orbit_camera(float dx, float dy);
        void pan_camera(float dx, float dy);
        void dolly_camera(float steps);
        void sync_state_to_renderer();
        bool handle_gizmo_click(const QPoint &pos);
        void set_view_axis(const QVector3D &dir);
        void move_camera_wsad();

        static constexpr int GIZMO_SIZE = 120;
        static constexpr int GIZMO_MARGIN = 12;

        std::string shader_dir_;
        ViewportState state_;
        std::vector<render::Mesh> meshes_;
        ViewportRenderer *renderer_ = nullptr;

        bool rotating_entity_ = false;
        bool orbiting_ = false;
        bool panning_ = false;
        QPoint last_pos_;

        bool key_w_ = false, key_s_ = false, key_a_ = false, key_d_ = false;
        QTimer wsad_timer_;
    };
}