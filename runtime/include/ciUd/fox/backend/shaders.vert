// Fox backend/shaders.vert - 实例化四边形顶点着色器
//
// Fox 的每个图元（矩形、圆角矩形、边框、阴影、glass）都是一个四边形。
// 一个顶点缓冲（单位四边形）+ 一个实例缓冲（每命令参数）。
// 顶点着色器把单位四边形展开成命令的像素矩形，并把局部坐标传给
// 片段着色器求值 SDF。

#version 450

// 每帧 uniform 块（std140）。Vulkan GLSL 要求显式 binding。
layout(set = 0, binding = 0) uniform Frame {
    vec2 resolution;
    float time;
    float pad;
} frame;

layout(location = 0) in vec2 in_unit;      // 0..1 单位四边形
layout(location = 1) in vec4 in_rect;      // x, y, w, h  （像素）
layout(location = 2) in vec4 in_radii;     // tl, tr, br, bl（px）
layout(location = 3) in vec4 in_color;     // rgba 0..1
layout(location = 4) in vec4 in_color2;    // rgba 0..1（渐变终止色）
layout(location = 5) in vec4 in_params;    // 类型, 线宽, 模糊, 色调
layout(location = 6) in vec4 in_params2;   // 饱和度, gradAxis, uv0.x, uv0.y

layout(location = 0) out vec2 v_local;     // 形状内的像素坐标
layout(location = 1) out vec4 v_rect;
layout(location = 2) out vec4 v_radii;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec4 v_color2;
layout(location = 5) out vec4 v_params;
layout(location = 6) out vec4 v_params2;
layout(location = 7) out vec2 v_screen;    // 绝对像素坐标（glass 模糊采样）
layout(location = 8) out vec2 v_uv;        // 矩形内 0..1 uv（文字采样）

void main()
{
    // 把单位四边形按像素展开到目标矩形。
    vec2 px = in_rect.xy + in_unit * in_rect.zw;

    // 局部坐标以矩形中心为原点，片段着色器据此求值对称的 SDF。
    vec2 local = (in_unit - 0.5) * in_rect.zw;

    // PTYPE_PATH = 6：实例的四边形是线段的包围盒，v_local 必须在"线段"
    // 坐标系里量，而不是盒坐标系。
    //
    // ⚠ 原因：实例化四边形是轴对齐的，斜线段没法用它自己的盒子表达。
    //   携带线段方向并旋转 v_local 才能让折线成为折线；不做这一步，每条
    //   斜线段都会渲染成横跨盒子的水平条（对勾会画成两短横，看起来像
    //   字体或数据问题，而不像缺一次旋转）。
    //
    //   片段着色器仍沿 x 量距离（sdLineSegment 的约定），所以这只是换基：
    //   两个点积，不用三角函数。
    if (in_params.x == 6.0)
    {
        vec2 dir = vec2(in_params.w, in_params2.w);
        local = vec2(dot(local, dir), dot(local, vec2(-dir.y, dir.x)));
    }
    v_local = local;
    // v_uv 把四边形映射到字形在字体图集中的子矩形。
    //
    // ⚠ 这里不要翻转 Y。GlyphQuad::uv0/uv1 已按 stb_truetype 产出的顺序
    //   携带图集空间的 V（第 0 行是字形顶部），图集也是逐行上传的，所以
    //   uv0.y 已指向字形上沿。在这里翻转会让每个字形原地倒过来——单词
    //   仍是从左到右排的，所以看起来像字体问题而不是 UV 问题。
    v_uv = in_unit;

    v_rect = in_rect;
    v_radii = in_radii;
    v_color = in_color;
    v_color2 = in_color2;
    v_params = in_params;
    v_params2 = in_params2;
    v_screen = px;

    // 屏幕坐标 -> 裁剪坐标。
    //
    // ⚠ 这里不要对 Y 取负。Vulkan 裁剪空间本身就是 +Y 向下，和帧缓冲空间
    //   一致：像素 (0,0) 在左上角，映射到 NDC (-1,-1)。取负会把整个 UI
    //   上下颠倒——而且不容易发现，因为整窗背景矩形是对称的，看起来仍然
    //   正常，而所有不对称的细节（标题栏、窗口按钮、文字基线）会悄悄
    //   跑到窗口的另一端。OpenGL 是相反的约定，别把那个习惯搬过来。
    vec2 ndc = vec2(
        (px.x / frame.resolution.x) * 2.0 - 1.0,
        (px.y / frame.resolution.y) * 2.0 - 1.0
    );
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
}