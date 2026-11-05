// Fox backend/shaders.frag - SDF 图元片段着色器
//
// 求值圆角矩形的有向距离场，生成抗锯齿的填充、边框、渐变和软阴影。
// Glass 额外采样一张模糊背景纹理。
//
// SDF 方案之所以开销低：圆角不花几何成本，边在任何缩放下都清晰，
// 抗锯齿来自距离场的连续性，而不是 MSAA。
//
// binding 3 是 Image 批次的纹理。它用独立 binding，而不是按实例索引的
// 数组，因为 Vulkan 要求聚合成数组的 sampler 必须用常量整型表达式索引
// （VUID-VkDescriptorSetLayoutBinding-descriptorType-06482）。Fox 改为按
// 纹理分批，与它按字体分批的方式一致。

#version 450

layout(set = 0, binding = 0) uniform Frame {
    vec2 resolution;
    float time;
    float pad;
} frame;

layout(set = 0, binding = 1) uniform sampler2D u_fontAtlas;  // 覆盖率（R8）
layout(set = 0, binding = 2) uniform sampler2D u_blurred;    // 模糊背景
layout(set = 0, binding = 3) uniform sampler2D u_tex;         // 图片源

// v_params.x 编码图元类型（见下面的 PTYPE_*）。

layout(location = 0) in vec2 v_local;
layout(location = 1) in vec4 v_rect;
layout(location = 2) in vec4 v_radii;
layout(location = 3) in vec4 v_color;
layout(location = 4) in vec4 v_color2;
layout(location = 5) in vec4 v_params;
layout(location = 6) in vec4 v_params2;
layout(location = 7) in vec2 v_screen;
layout(location = 8) in vec2 v_uv;

layout(location = 0) out vec4 outColor;

// 图元类型
const float PTYPE_RECT   = 0.0;
const float PTYPE_SHADOW = 1.0;
const float PTYPE_BORDER = 2.0;
const float PTYPE_GLASS  = 3.0;
const float PTYPE_TEXT   = 4.0;
const float PTYPE_IMAGE  = 5.0;
const float PTYPE_PATH   = 6.0;
const float PTYPE_ELLIPSE = 7.0;
const float PTYPE_TRIANGLE = 8.0;

// 到以原点为中心的圆角矩形的有向距离，半尺寸 `b`，
// 四角半径 r = (左上, 右上, 右下, 左下)。
float sdRoundBox(vec2 p, vec2 b, vec4 r)
{
    r.xy = (p.x > 0.0) ? r.yz : r.xw;   // x 选右对还是左对
    r.x  = (p.y > 0.0) ? r.y : r.x;     // y 选下还是上

    vec2 q = abs(p) - b + r.x;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}

float sdRoundBoxUniform(vec2 p, vec2 b, float r)
{
    r = min(r, min(b.x, b.y));
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// 到线段的有向距离，端头样式可选。
// p: 局部坐标中的点（线段在 x 轴上，从 -half_len 到 +half_len）
// half_len: 线段长度的一半
// half_thick: 描边宽度的一半
// cap_style: 0=平头, 1=圆头, 2=方头
float sdLineSegment(vec2 p, float half_len, float half_thick, int cap_style)
{
    // 投影到线段上
    float t = clamp(p.x, -half_len, half_len);
    float d = length(p - vec2(t, 0.0));
    
    // 按端头样式处理
    if (cap_style == 1) // 圆头
        return d - half_thick;
    else if (cap_style == 2) // 方头
    {
        float dx = max(abs(p.x) - half_len, 0.0);
        return max(dx, d) - half_thick;
    }
    // 平头（默认）
    float dx = max(abs(p.x) - half_len, 0.0);
    if (dx > 0.0) return sqrt(dx * dx + d * d) - half_thick;
    return d - half_thick;
}

// 用局部四边形坐标求到路径（折线）的有向距离。
//
// v_local 已在"线段"坐标系里（顶点着色器做了旋转；见 shaders.vert），
// 所以仍是单线段求值。
// v_params.y = 线宽, v_params.z = 线段长度,
// v_params2.x = 端头, v_params2.y = 拐角, v_params2.z = 标志位
float sdPath(vec2 p, float half_len, float half_thick, int cap_style, int join_style)
{
    // 目前各线段独立处理，统一用圆端头/圆拐角。
    // 四边形本身就是线段，直接用 sdLineSegment。
    return sdLineSegment(p, half_len, half_thick, cap_style);
}

// 到过 p0,p1 的直线的有向距离，多边形外部为正；
// 环绕方向 sign(a) = +1 为正向，否则为 -1。
//
// 为什么用直线而不是线段：对凸多边形，精确有向距离就是各边半平面距离
// 的 max()——每条边的直线是其前方点的最近边界，max 会选出那条边。在角上
// 两个半平面等距，max 的取法与真距离沿对角线的行为一致（在边界和 ±1px
// 抗锯齿带内距离是精确的；角部再往外最多低估一个圆角量，这是凸多边形
// SDF 的标准行为）。
float sdEdge(vec2 p, vec2 p0, vec2 p1, float s)
{
    vec2 e = p1 - p0;
    float len = length(e);
    if (len < 0.000001) return -1.0e9; // 退化边不产生约束
    return -s * (e.x * (p.y - p0.y) - e.y * (p.x - p0.x)) / len;
}

void main()
{
    float ptype = v_params.x;
    vec2 half_ext = v_rect.zw * 0.5;

    if (ptype == PTYPE_TEXT)
    {
        // uv0 在 params2.zw，uv1 在 color2.xy（color2 对文字四边形复用为
        // uv1 载体——Text 命令没有渐变终止色）。
        vec2 uv = mix(v_params2.zw, v_color2.xy, v_uv);
        float cov = texture(u_fontAtlas, uv).r;
        // 丢弃完全透明的纹素，避免重叠文字重复混合。
        if (cov < 0.004) discard;
        outColor = vec4(v_color.rgb, v_color.a * cov);
        return;
    }

    if (ptype == PTYPE_IMAGE)
    {
        // v_uv 在四边形上取 0..1；color2.xyzw 是归一化的源矩形
        // (u0, v0, u1, v1)。整张贴图绘制传 {0,0,1,1}。
        //
        // 纹理被拉伸填满四边形。UI 资源正需要这样：图标是按槽位制作的，
        // 不是按固定像素尺寸。
        vec2 uv = mix(v_color2.xy, v_color2.zw, v_uv);
        vec4 t = texture(u_tex, uv);
        // 用乘法而非直接替换：白色原样显示纹理，颜色给它上色（单色图标
        // 就是这样染上主题色的），alpha 控制淡出。直通 alpha——预乘由
        // 混合状态完成。
        vec4 c = t * v_color;
        if (c.a < 0.003) discard;
        outColor = c;
        return;
    }

    if (ptype == PTYPE_PATH)
    {
        // 路径：v_local 已在段坐标系里（顶点着色器旋转过），
        // 距离就是前面的 sdLineSegment。
        // v_params.y = 线宽, v_params.z = 线段长度,
        // v_params2.x = 端头, v_params2.y = 拐角
        float half_len = v_params.z * 0.5;
        float half_thick = v_params.y * 0.5;

        float d = sdPath(v_local, half_len, half_thick, int(v_params2.x), int(v_params2.y));

        float a = 1.0 - smoothstep(-0.5, 0.5, d);
        if (a < 0.003) discard;
        outColor = vec4(v_color.rgb, v_color.a * a);
        return;
    }

    if (ptype == PTYPE_ELLIPSE)
    {
        // 边界上精确的椭圆 SDF：k1*(k1-1)/k2，其中 q = p/r 把椭圆映射为
        // 单位圆。圆时精确退化为 |p| - r，可作校验。
        //
        // 离开边界后梯度是近似的（AE 近似精确场），但偏差在离边 ±1px
        // 之外；抗锯齿渐变带在 ±1px 内且那里是精确的，smoothstep 窗口
        // 只看这一带。
        vec2 r = max(half_ext, vec2(0.001));
        vec2 q = v_local / r;
        float k1 = length(q);
        float k2 = length(q / r);
        float d = k2 > 0.000001 ? k1 * (k1 - 1.0) / k2 : k1 - 1.0;

        float a = 1.0 - smoothstep(-0.5, 0.5, d);
        if (a < 0.003) discard;
        outColor = vec4(v_color.rgb, v_color.a * a);
        return;
    }

    if (ptype == PTYPE_TRIANGLE)
    {
        // 凸多边形填充：三条边有向距离取 max()。
        // 顶点是相对矩形中心的坐标（与 v_local 同原点），环绕符号在这里算，
        // 调用方正序逆序给顶点都行。
        vec2 a = v_params.yz;
        vec2 b = vec2(v_params.w, v_params2.x);
        vec2 c = v_params2.yz;
        float orient = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        float s = orient > 0.0 ? 1.0 : -1.0;
        float d = max(max(sdEdge(v_local, a, b, s), sdEdge(v_local, b, c, s)),
                      sdEdge(v_local, c, a, s));

        float alpha = 1.0 - smoothstep(-0.5, 0.5, d);
        if (alpha < 0.003) discard;
        outColor = vec4(v_color.rgb, v_color.a * alpha);
        return;
    }

    float d;
    bool uniform_radii = (abs(v_radii.x - v_radii.y) < 0.001) &&
                         (abs(v_radii.y - v_radii.z) < 0.001);
    if (uniform_radii)
        d = sdRoundBoxUniform(v_local, half_ext, v_radii.x);
    else
        d = sdRoundBox(v_local, half_ext, v_radii);

    // 软阴影：形状外宽而低透明度的衰减。
    if (ptype == PTYPE_SHADOW)
    {
        // ⚠ build_instances() 已经把四边形四周各扩了 `blur`，所以 `d` 是
        //   相对"外扩后的盒子"量的，不是相对形状本身。几何就是这么定的，
        //   整个渐变由此决定：
        //
        //     d == 0     恰好在四边形边界上
        //     d == -blur 恰好在形状真实边缘上
        //     d <  -blur 在形状下方
        //
        //   形状不透明地盖在上面，可见的只有 d == 0 到 d == -blur 之间
        //   这一带，且往里要从 0 升到 1。
        //
        //   看似对称的 `1 - smoothstep(-blur, blur, d)` 两处都错：d 在
        //   四边形内永远到不了 +blur，边界上只到 a == 0.5 然后硬切——
        //   留下一圈轮廓锋利的不透明黑带。而 `smoothstep(0, blur, d)`
        //   方向反了：d 处处 <= 0，返回值处处为 0，阴影整个消失。
        float blur = max(v_params.z, 0.5);
        float a = 1.0 - smoothstep(-blur, 0.0, d);
        if (a < 0.003) discard;
        outColor = vec4(v_color.rgb, v_color.a * a);
        return;
    }

    if (ptype == PTYPE_BORDER)
    {
        float t = max(v_params.y, 0.5);
        float half_t = t * 0.5;
        float a = 1.0 - smoothstep(half_t - 0.5, half_t + 0.5, abs(d));
        if (a < 0.003) discard;
        outColor = vec4(v_color.rgb, v_color.a * a);
        return;
    }

    if (ptype == PTYPE_GLASS)
    {
        // 磨砂材质：采样预模糊的背景，提升饱和度、叠加色调，再加一圈高光
        // 让边缘反光。
        vec2 uv = v_screen / frame.resolution;
        float sat = v_params2.x;
        vec3 back = texture(u_blurred, uv).rgb;

        float lum = dot(back, vec3(0.2126, 0.7152, 0.0722));
        back = mix(vec3(lum), back, sat);

        float tint_amt = v_params.w;
        vec3 glass = mix(back, v_color.rgb, tint_amt);

        // 边缘附近的高光（d < 0 = 形状内部）。
        float rim = 1.0 - smoothstep(-2.5, 0.0, d);
        glass += rim * 0.14;

        float a = 1.0 - smoothstep(-0.5, 0.5, d);
        outColor = vec4(glass, v_color.a * a);
        return;
    }

    // PTYPE_RECT：纯色或线性渐变填充。
    float a = 1.0 - smoothstep(-0.5, 0.5, d);
    if (a < 0.003) discard;
    vec3 col;
    if (v_color2.a > 0.0)
    {
        // 沿轴向渐变：v_params2.y = 0 竖直，1 水平。
        float t = clamp((v_local.y + half_ext.y) / (2.0 * half_ext.y), 0.0, 1.0);
        if (v_params2.y > 0.5)
            t = clamp((v_local.x + half_ext.x) / (2.0 * half_ext.x), 0.0, 1.0);
        col = mix(v_color.rgb, v_color2.rgb, t);
    }
    else
    {
        col = v_color.rgb;
    }
    outColor = vec4(col, v_color.a * a);
}