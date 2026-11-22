#version 450

layout(set = 0, binding = 0) uniform Frame {
    vec2 resolution;
    float time;
    float pad;
} frame;

layout(set = 0, binding = 1) uniform sampler2D u_fontAtlas;
layout(set = 0, binding = 2) uniform sampler2D u_blurred;
layout(set = 0, binding = 3) uniform sampler2D u_tex;


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

const float PTYPE_RECT   = 0.0;
const float PTYPE_SHADOW = 1.0;
const float PTYPE_BORDER = 2.0;
const float PTYPE_GLASS  = 3.0;
const float PTYPE_TEXT   = 4.0;
const float PTYPE_IMAGE  = 5.0;
const float PTYPE_PATH   = 6.0;
const float PTYPE_ELLIPSE = 7.0;
const float PTYPE_TRIANGLE = 8.0;

float sdRoundBox(vec2 p, vec2 b, vec4 r)
{
    r.xy = (p.x > 0.0) ? r.yz : r.xw;
    r.x = (p.y > 0.0) ? r.y : r.x;

    vec2 q = abs(p) - b + r.x;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}

float sdRoundBoxUniform(vec2 p, vec2 b, float r)
{
    r = min(r, min(b.x, b.y));
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float sdLineSegment(vec2 p, float half_len, float half_thick, int cap_style)
{
    float t = clamp(p.x, -half_len, half_len);
    float d = length(p - vec2(t, 0.0));
    
    if (cap_style == 1)
        return d - half_thick;
    else if (cap_style == 2)
    {
        float dx = max(abs(p.x) - half_len, 0.0);
        return max(dx, d) - half_thick;
    }
    float dx = max(abs(p.x) - half_len, 0.0);
    if (dx > 0.0) return sqrt(dx * dx + d * d) - half_thick;
    return d - half_thick;
}

float sdPath(vec2 p, float half_len, float half_thick, int cap_style, int join_style)
{
    return sdLineSegment(p, half_len, half_thick, cap_style);
}

float sdEdge(vec2 p, vec2 p0, vec2 p1, float s)
{
    vec2 e = p1 - p0;
    float len = length(e);
    if (len < 0.000001) return -1.0e9;
    return -s * (e.x * (p.y - p0.y) - e.y * (p.x - p0.x)) / len;
}

void main()
{
    float ptype = v_params.x;
    vec2 half_ext = v_rect.zw * 0.5;

    if (ptype == PTYPE_TEXT)
    {
        vec2 uv = mix(v_params2.zw, v_color2.xy, v_uv);
        float cov = texture(u_fontAtlas, uv).r;
        if (cov < 0.004) discard;
        outColor = vec4(v_color.rgb, v_color.a * cov);
        return;
    }

    if (ptype == PTYPE_IMAGE)
    {
        vec2 uv = mix(v_color2.xy, v_color2.zw, v_uv);
        vec4 t = texture(u_tex, uv);
        vec4 c = t * v_color;
        if (c.a < 0.003) discard;
        outColor = c;
        return;
    }

    if (ptype == PTYPE_PATH)
    {
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

    if (ptype == PTYPE_SHADOW)
    {
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
        vec2 uv = v_screen / frame.resolution;
        float sat = v_params2.x;
        vec3 back = texture(u_blurred, uv).rgb;

        float lum = dot(back, vec3(0.2126, 0.7152, 0.0722));
        back = mix(vec3(lum), back, sat);

        float tint_amt = v_params.w;
        vec3 glass = mix(back, v_color.rgb, tint_amt);

        float rim = 1.0 - smoothstep(-2.5, 0.0, d);
        glass += rim * 0.14;

        float a = 1.0 - smoothstep(-0.5, 0.5, d);
        outColor = vec4(glass, v_color.a * a);
        return;
    }

    float a = 1.0 - smoothstep(-0.5, 0.5, d);
    if (a < 0.003) discard;
    vec3 col;
    if (v_color2.a > 0.0)
    {
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