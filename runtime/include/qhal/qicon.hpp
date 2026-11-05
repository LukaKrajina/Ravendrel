// ═══════════════════════════════════════════════════════════════════════
// Bro 图标库 —— 程序化 SDF 光栅化
// ═══════════════════════════════════════════════════════════════════════
//
// 不用图片文件的原因：
//   · 桌面端要的是「矢量感」的图标 —— 任意缩放都清晰，深色主题下能整体调色
//   · 位图图标在 1x/2x/3x 下要么模糊要么要备三套资源
//   · 更要命的是：Nuklear 的 nk_draw_image 需要一个 Vulkan 纹理句柄，
//     而创建纹理的代码（nk_glfw3_device_upload_atlas）是头文件里的
//     NK_INTERN，外部模块调不到。为几张图标去复制 100 行 Vulkan 样板，
//     不划算。
//
// 所以走 **SDF（有符号距离场）** 路线：
//   · 每个图标是 H×W 的 float 张量，值 = 到形状边界的带符号距离
//   · 形状用解析式求值，天然矢量、任意分辨率都清晰
//   · 光栅化时按**输出像素**求覆盖率，再把同行连续像素合并成水平条带，
//     每条带一个 nk_fill_rect —— 一条命令，不是几百个三角形
//   · 抗锯齿来自 SDF 本身的连续性：coverage = clamp(0.5 - d, 0, 1)
//
// ── 「张量矩阵」体现在哪 ────────────────────────────────────────────────
//
// 1. **场就是张量**：H×W 的 float 矩阵，每个元素是一个振幅。
// 2. **图层组合是矩阵运算**：并/交/差对应逐元素 min / max / max(a,-b)，
//    平滑并是多项式核 k 的矩阵形式。这套组合代数是 SDF 的核心，
//    也是「一个图标 = 多个形状叠加」能成立的数学基础。
// 3. **仿射变换是 2×3 矩阵**：旋转/缩放/平移写成矩阵，
//    对场做逆变换采样（双线性插值），图标就能旋转和拉伸。
// 4. **颜色是 3×3 矩阵 + 偏移**：调色板变换（色相旋转、亮度、对比度）
//    用矩阵乘加实现，换主题色不需要重新生成图标。
//
// ── 关于「量子」 ────────────────────────────────────────────────────────
//
// 这里**不声称**有量子加速。场的组合代数与量子态的叠加在数学上同构
// （都是向量空间上的线性组合），但这个库是确定性的经典计算，
// 跑在 CPU 上，复杂度 O(H·W)。
// 把它叫「量子图标库」是过度宣传且没必要考虑量子计算跑图标，没必要那是经典该干的事。
// 所以文档里只说「SDF + 张量组合」。
//
// 编译目标：随 rvdl_rt 一起构建。

#pragma once
#include "RuntimeApi.h"
#include "nuklear_config.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

// ═══════════════════════════════════════════════════════════════════════
// 场
// ═══════════════════════════════════════════════════════════════════════

/// 有符号距离场。d[i] 是到最近形状边界的距离（负 = 内部）。
/// 固定 32×32 —— 图标不需要更高分辨率，且能让所有图标共用一套缓存。
#define QICON_SIZE 32
#define QICON_CELLS (QICON_SIZE * QICON_SIZE)

typedef struct qicon_field
{
    int32_t w, h;
    float d[QICON_CELLS];
} qicon_field;

/// 清空为「全空」（距离 = +∞ 用一个大正数表示）
RVDL_HOST_EXPORT void qicon_clear(qicon_field *f);

/// 用常量填充（用于调试与底色）
RVDL_HOST_EXPORT void qicon_fill(qicon_field *f, float v);

// ═══════════════════════════════════════════════════════════════════════
// 形状原语（把 SDF 写进 f，返回 1 表示成功）
// ═══════════════════════════════════════════════════════════════════════

RVDL_HOST_EXPORT int32_t qicon_rect(qicon_field *f, float x, float y, float w, float h, float r);
RVDL_HOST_EXPORT int32_t qicon_circle(qicon_field *f, float cx, float cy, float r);
RVDL_HOST_EXPORT int32_t qicon_ring(qicon_field *f, float cx, float cy, float r, float thickness);
RVDL_HOST_EXPORT int32_t qicon_segment(qicon_field *f, float x0, float y0, float x1, float y1,
                                        float thickness);
RVDL_HOST_EXPORT int32_t qicon_triangle(qicon_field *f, float x0, float y0, float x1, float y1,
                                         float x2, float y2);
RVDL_HOST_EXPORT int32_t qicon_star(qicon_field *f, float cx, float cy, float r_outer, float r_inner,
                                     int32_t points, float rot);
RVDL_HOST_EXPORT int32_t qicon_polygon(qicon_field *f, const float *pts, int32_t count);

// ═══════════════════════════════════════════════════════════════════════
// 组合代数（逐元素矩阵运算）
// ═══════════════════════════════════════════════════════════════════════

/// 并集：min(a, b)
RVDL_HOST_EXPORT int32_t qicon_union(qicon_field *a, const qicon_field *b);
/// 差集：max(a, -b)
RVDL_HOST_EXPORT int32_t qicon_subtract(qicon_field *a, const qicon_field *b);
/// 交集：max(a, b)
RVDL_HOST_EXPORT int32_t qicon_intersect(qicon_field *a, const qicon_field *b);
/// 平滑并集：多项式核，k 越大过渡越宽
RVDL_HOST_EXPORT int32_t qicon_smooth_union(qicon_field *a, const qicon_field *b, float k);

// ═══════════════════════════════════════════════════════════════════════
// 仿射变换（2×3 矩阵，逆变换 + 双线性插值采样）
// ═══════════════════════════════════════════════════════════════════════

/// m = [a b c; d e f]，表示 x' = a*x + b*y + c, y' = d*x + e*y + f
RVDL_HOST_EXPORT int32_t qicon_transform(qicon_field *dst, const qicon_field *src, const float m[6]);

/// 便捷：绕 (cx,cy) 旋转 angle 弧度
RVDL_HOST_EXPORT int32_t qicon_rotate(qicon_field *dst, const qicon_field *src, float cx, float cy,
                                        float angle);
/// 便捷：以 (cx,cy) 为中心缩放
RVDL_HOST_EXPORT int32_t qicon_scale(qicon_field *dst, const qicon_field *src, float cx, float cy,
                                       float sx, float sy);

// ═══════════════════════════════════════════════════════════════════════
// 颜色（3×3 矩阵 + 偏移）
// ═══════════════════════════════════════════════════════════════════════

/// 调色板变换：out = M * in + off，逐通道。
/// M 是行主序 3×3，off 是 3 元偏移。
RVDL_HOST_EXPORT void qicon_color_transform(int32_t out[3], const int32_t in[3], const float M[9],
                                              const float off[3]);

/// 色相旋转（角度制）+ 亮度缩放 + 对比度。返回 0xRRGGBB。
RVDL_HOST_EXPORT int32_t qicon_tint(int32_t rgb, float hue_deg, float lightness, float contrast);

// ═══════════════════════════════════════════════════════════════════════
// 光栅化
// ═══════════════════════════════════════════════════════════════════════

/// 把场画到命令缓冲：逐行找覆盖率 > 0 的连续段，每段一个 nk_fill_rect。
/// 返回发出的矩形数（0 = 图标是空的）。
///
/// (x, y, w, h) 是目标矩形（屏幕像素），场会被拉伸到该矩形。
/// alpha_scale 用于整体调淡（例如禁用态）。
RVDL_HOST_EXPORT int32_t qicon_draw(nk_command_buffer *cb, float x, float y, float w, float h,
                                      const qicon_field *f, int32_t color, float alpha_scale);

/// 只算会画多少个矩形（布局预排用，不产生命令）
RVDL_HOST_EXPORT int32_t qicon_draw_count(const qicon_field *f);

// ═══════════════════════════════════════════════════════════════════════
// 命名图标
// ═══════════════════════════════════════════════════════════════════════

/// 图标 id。新增图标就在这里加一行，并在 qicon_build 里实现。
enum
{
    QICON_SEND = 0,     // 发送（纸飞机）
    QICON_ATTACH,       // 附加（回形针）
    QICON_SEARCH,       // 搜索（放大镜）
    QICON_SETTINGS,     // 设置（齿轮）
    QICON_USER,         // 用户（人像）
    QICON_TOOL,         // 工具（扳手）
    QICON_WARN,         // 警告（三角叹号）
    QICON_OK,           // 成功（圆勾）
    QICON_FAIL,         // 失败（圆叉）
    QICON_CHIMERA,      // Chimera（叠加双环）
    QICON_PLAN,         // 计划（清单）
    QICON_VERIFY,       // 验证（盾牌勾）
    QICON_BOT,          // 智能体（机器人）
    QICON_HISTORY,      // 历史（时钟）
    QICON_SKILL,        // Skill（闪电）
    QICON_BRAND,        // 品牌（黑乌鸦头 —— OpenButler/qk 的标记）
    QICON_COUNT
};

/// 生成第 id 个图标的场。越界返回 0。
RVDL_HOST_EXPORT int32_t qicon_build(int32_t id, qicon_field *out);

/// 图标默认尺寸（像素）
RVDL_HOST_EXPORT int32_t qicon_default_size(int32_t id);

// ═══════════════════════════════════════════════════════════════════════
// 组合算子（供 rvdl_icon_mix 用）
// ═══════════════════════════════════════════════════════════════════════

enum
{
    QICON_OP_UNION = 0,      ///< 并集：min(a, b)
    QICON_OP_SUBTRACT = 1,   ///< 差集：max(a, -b)
    QICON_OP_INTERSECT = 2,  ///< 交集：max(a, b)
    QICON_OP_SMOOTH = 3,     ///< 平滑并集：多项式核
};