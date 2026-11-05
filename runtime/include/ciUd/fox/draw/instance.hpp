#pragma once
// Fox draw/instance.hpp - 显示列表 -> 实例，与后端无关
//
// 这是"画什么"和"怎么送到屏幕"之间的接缝。它以前在
// VkBackend::build_instances 里面，意味着只有有 Vulkan 设备才能渲染一帧
// ——无头渲染不了，QuarkOS（没有 GPU、没有显示驱动、没有 libc 的独立
// 内核）上更是完全没法渲染。
//
// 两个后端消费的都是这个：
//   VulkanBackend  -> 上传实例，按实例化四边形绘制
//   CpuBackend     -> 在 CPU 上把它们光栅化进 RGBA8 缓冲
//
// 因为两者消费同一个缓冲，CPU 路径兼作 GLSL 的参考实现：两者不一致时
// 必有一个错了，而且有具体办法查出是哪个。

#include "core/context.hpp"
#include "draw/display_list.hpp"

namespace fox
{
    /// 上传给 GPU / 供光栅化器消费的单实例数据。
    /// 与着色器的实例属性对应：6 x vec4 = 96 字节。
    ///
    /// 字段复用是刻意的，而且是承重设计。这里没有 24 个空闲的 float，
    /// 假装有就会写出字形重叠的 bug。当前含义：
    ///
    ///   rect    x, y, w, h                 逻辑单位
    ///   radii   tl, tr, br, bl             逻辑像素
    ///   color   rgba                       着色
    ///   color2  TEXT: uv1.xy               （uv0 在 params2.zw）
    ///           IMAGE: src rect u0,v0,u1,v1
    ///           其他: 渐变终点色
    ///   params  x: 图元类型                见 backend/vk_backend.cpp 的 PT_*
    ///           y: 边框粗细
    ///           z: 阴影模糊
    ///           w: 玻璃着色
    ///   params2 x: 玻璃饱和度
    ///           y: 渐变方向
    ///           zw: 文本 uv0
    ///
    /// PT_PATH 额外读：params.y 粗细、params.z 线段长度、params.w
    /// direction.x、params2.x 端点样式、params2.y 连接样式、params2.z
    /// 标志、params2.w direction.y——rect 是线段的包围盒而不是线段本身的
    /// 盒子，因为着色器会把 v_local 旋转到线段自己的坐标系（斜线段是旋转
    /// 后的矩形，轴对齐四边形自己表达不了）。
    ///
    /// PT_TRIANGLE 额外读：params.yz / params.w+params2.x / params2.yz
    /// = 相对矩形中心的三个顶点，与 v_local 用同一个原点。
    struct InstanceData
    {
        f32 rect[4];
        f32 radii[4];
        f32 color[4];
        f32 color2[4];
        f32 params[4];
        f32 params2[4];
    };

    /// 两个后端共用的图元类型。
    inline constexpr f32 PT_RECT = 0.0f;
    inline constexpr f32 PT_SHADOW = 1.0f;
    inline constexpr f32 PT_BORDER = 2.0f;
    inline constexpr f32 PT_GLASS = 3.0f;
    inline constexpr f32 PT_TEXT = 4.0f;
    inline constexpr f32 PT_IMAGE = 5.0f;
    inline constexpr f32 PT_PATH = 6.0f;
    inline constexpr f32 PT_ELLIPSE = 7.0f;
    inline constexpr f32 PT_TRIANGLE = 8.0f;

    /// 一段连续的、需要相同绑定纹理的实例。
    ///
    /// 是"分段"而不是"所有形状排完再排文本"：实例做 alpha 混合、不做
    /// 深度测试，绘制顺序就是绘制次序，把文本统一排到最后会让标签盖在
    /// 画在它之后的面板上面。
    ///
    /// key 是（字体，纹理）——实例需要的绑定——绝不是描述符集下标。
    /// Vulkan 禁止按实例索引采样器数组，换纹理确实意味着换集，但集的
    /// 下标由只增不减的后端槽位 vector 推导，把它缓存在这里就会出现
    /// 批次按一个下标记录、绑定时槽位已经挪走的情况。
    struct Batch
    {
        u32 first = 0;
        u32 count = 0;
        FontId font = FONT_INVALID; ///< FONT_INVALID = 不需要字形图集
        TexId tex = TEX_NONE;      ///< TEX_NONE = 不需要图像纹理
        /// 本批次生效的裁剪区域，单位与实例矩形相同。空矩形表示"未裁剪"。
        ///
        /// 批次从不跨裁剪边界：build_instances 在每个 ClipPush/ClipPop 处
        /// 都会冲刷当前段，所以每批次一个裁剪矩形是精确的。
        Rect clip;
    };

    /// 一帧的可绘制几何。
    struct InstanceSet
    {
        std::vector<InstanceData> instances;
        std::vector<Batch> batches;
        bool used_glass = false; ///< 本帧需要模糊背景链
        u32 dropped = 0;         ///< 因空间不足跳过的命令
        /// 被后面完全盖住的不透明实例剔除掉的实例（见 draw/instance.cpp）。
        /// 纯统计量：剔不剔像素都一样，这正是剔除测试所断言的。
        u32 culled = 0;

        void clear()
        {
            instances.clear();
            batches.clear();
            used_glass = false;
            dropped = 0;
            culled = 0;
        }

        size_t bytes() const { return instances.size() * sizeof(InstanceData); }
    };

    /// 把显示列表转换成实例和批次。
    ///
    /// 对任何后端都是纯的：它只读 Context（图集可用性和 DPI 缩放），
    /// 不读别的。它不知道纹理是否已经进了 VRAM——各后端自行丢掉暂时
    /// 画不了的东西。
    ///
    /// 坐标输出为逻辑单位。乘 DPI 是后端的活，所以 Vulkan 路径里只有一
    /// 行，CPU 路径则直接忽略。
    void build_instances(Context &c, const DisplayList &dl, InstanceSet &out);

} // namespace fox
