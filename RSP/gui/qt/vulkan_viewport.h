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

    // 单个实例的变换
    struct InstanceTransform
    {
        int mesh_id = 0;
        QVector3D position;
        QQuaternion rotation;
        QVector3D scale{1, 1, 1};
    };

    // 每帧视口状态
    struct ViewportState
    {
        float camera_yaw = -0.8f;
        float camera_pitch = 0.45f;
        float camera_dist = 14.0f;
        float camera_target[3] = {0.0f, 1.0f, 0.0f}; // 环绕中心（pivot，UE5 风格）
        float sun_dir[3] = {0.35f, 0.75f, 0.45f}; // 太阳方向（大气散射用）
        bool ortho = false;                       // 正交投影（gizmo 切换视角时启用）
        bool dark_mode = true;                    // 白天/夜间（影响 clear color 与光照）
        std::vector<InstanceTransform> instances;
    };

    // Vulkan 渲染器
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

        // GPU 顶点（float 布局，与管线属性 R32G32B32/R32G32 严格对齐）
        // 注意：render::Vertex 的 position/normal 是 qpc::Vec3（double，24B），
        // 不能直接上传，必须在 upload_meshes() 里转成 float。
        struct GpuVertex
        {
            float px, py, pz;
            float nx, ny, nz;
            float u, v;
            float r, g, b;
        };

        // mesh UBO（光照/材质等共享参数，mvp/model 走 push constant）
        struct alignas(16) Ubo
        {
            float light_dir[3];
            float light_intensity;
            float cam_pos[3];
            float dark_mode; // 0 = 白天, 1 = 夜间（shader 内做氛围混合）
            float base_color[3];
            float metallic;
            float roughness;
            float use_texture;
        };

        // 每实例 push constant（mvp + model）
        struct PushBlock
        {
            float mvp[16];
            float model[16];
        };

        // 天空 UBO（std140：mat4 + vec4 + vec4 = 96 字节，用 vec4 避免 vec3 对齐问题）
        struct alignas(16) SkyUbo
        {
            float inv_view_proj[16];
            float sun_dir[4]; // xyz = 方向, w = 强度
            float cam_pos[4]; // xyz = 位置, w = dark_mode(0=白天,1=夜间)
        };

        // 线框顶点（位置 + 颜色）
        struct LineVertex
        {
            float px, py, pz;
            float r, g, b;
        };

        // 线框 UBO（MVP + 颜色调制；网格线/gizmo 共用，gizmo tint 恒为 1）
        struct alignas(16) LineUbo
        {
            float mvp[16];
            float tint[4]; // xyz = 颜色调制系数，w = 保留
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

        // 天空管线（全屏三角形 + 大气散射）
        VkPipelineLayout sky_pipeline_layout_ = VK_NULL_HANDLE;
        VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout sky_desc_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool sky_desc_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet sky_desc_set_ = VK_NULL_HANDLE;
        VkBuffer sky_uniform_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory sky_uniform_mem_ = VK_NULL_HANDLE;

        // 线框管线（坐标系 + 网格线，LINE_LIST）
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

        // 视口导航 gizmo（右上角 XYZ 轴 + 立方体框）
        VkBuffer gizmo_vb_ = VK_NULL_HANDLE;
        VkDeviceMemory gizmo_vb_mem_ = VK_NULL_HANDLE;
        uint32_t gizmo_vertex_count_ = 0;
        // gizmo 独立 UBO（避免与网格线共享 line_uniform_mem_ 导致覆盖）
        VkBuffer gizmo_uniform_buf_ = VK_NULL_HANDLE;
        VkDeviceMemory gizmo_uniform_mem_ = VK_NULL_HANDLE;
        VkDescriptorSet gizmo_desc_set_ = VK_NULL_HANDLE;

        std::vector<render::Mesh> meshes_; // CPU 网格（待上传）
        std::vector<MeshGpu> gpu_meshes_;  // GPU 网格
        bool rebuild_meshes_ = false;

        static VkShaderModule load_shader(VkDevice dev, const std::string &path);
        static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t filter, VkMemoryPropertyFlags props);
        void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data,
                           VkBuffer &buf, VkDeviceMemory &mem);
        void destroy_mesh(MeshGpu &m);
    };

    // QVulkanWindow 子类
    class QVulkanViewport : public QVulkanWindow
    {
        Q_OBJECT
    public:
        QVulkanViewport(const std::string &shader_dir);
        void set_state(const ViewportState &s);
        void set_meshes(const std::vector<render::Mesh> &meshes);
        // 每帧只更新场景实例与太阳，不覆盖相机（相机由视口交互维护）
        void set_scene(const std::vector<InstanceTransform> &instances,
                       const float sun_dir[3]);
        void set_dark_mode(bool dark);       // 白天/夜间切换（背景色/光照联动）
        void reset_camera();                 // 重置到默认视角
        QVulkanWindowRenderer *createRenderer() override;
        // 获取相机朝向（前/右/上），供外部把屏幕拖拽转成世界空间操作
        void get_camera_vectors(QVector3D &fwd, QVector3D &right, QVector3D &up) const;

    signals:
        void dragDelta(float dx, float dy); // 左键拖拽增量（由当前变换模式决定操作）

    protected:
        void mousePressEvent(QMouseEvent *event) override;
        void mouseMoveEvent(QMouseEvent *event) override;
        void mouseReleaseEvent(QMouseEvent *event) override;
        void wheelEvent(QWheelEvent *event) override;
        void keyPressEvent(QKeyEvent *event) override;
        void keyReleaseEvent(QKeyEvent *event) override;

    private:
        void orbit_camera(float dx, float dy);   // 右键：环绕
        void pan_camera(float dx, float dy);     // 中键：平移
        void dolly_camera(float steps);          // 滚轮：缩放
        void sync_state_to_renderer();
        bool handle_gizmo_click(const QPoint &pos); // 处理 gizmo 点击，命中返回 true
        void set_view_axis(const QVector3D &dir);   // 切换到指定轴方向的正交视图
        void move_camera_wsad();                    // WASD 前后左右移动（由定时器驱动）

        // gizmo 屏幕区域常量（右上角）
        static constexpr int GIZMO_SIZE = 120;
        static constexpr int GIZMO_MARGIN = 12;

        std::string shader_dir_;
        ViewportState state_;
        std::vector<render::Mesh> meshes_;
        ViewportRenderer *renderer_ = nullptr;

        bool rotating_entity_ = false; // 左键
        bool orbiting_ = false;        // 右键
        bool panning_ = false;         // 中键
        QPoint last_pos_;

        // WASD 按键状态 + 移动定时器
        bool key_w_ = false, key_s_ = false, key_a_ = false, key_d_ = false;
        QTimer wsad_timer_;
    };
}