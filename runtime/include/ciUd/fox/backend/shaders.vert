#version 450

layout(set = 0, binding = 0) uniform Frame {
    vec2 resolution;
    float time;
    float pad;
} frame;

layout(location = 0) in vec2 in_unit;
layout(location = 1) in vec4 in_rect;
layout(location = 2) in vec4 in_radii;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec4 in_color2;
layout(location = 5) in vec4 in_params;
layout(location = 6) in vec4 in_params2;

layout(location = 0) out vec2 v_local;
layout(location = 1) out vec4 v_rect;
layout(location = 2) out vec4 v_radii;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec4 v_color2;
layout(location = 5) out vec4 v_params;
layout(location = 6) out vec4 v_params2;
layout(location = 7) out vec2 v_screen;
layout(location = 8) out vec2 v_uv;

void main()
{
    vec2 px = in_rect.xy + in_unit * in_rect.zw;

    vec2 local = (in_unit - 0.5) * in_rect.zw;

    if (in_params.x == 6.0)
    {
        vec2 dir = vec2(in_params.w, in_params2.w);
        local = vec2(dot(local, dir), dot(local, vec2(-dir.y, dir.x)));
    }
    v_local = local;
    v_uv = in_unit;

    v_rect = in_rect;
    v_radii = in_radii;
    v_color = in_color;
    v_color2 = in_color2;
    v_params = in_params;
    v_params2 = in_params2;
    v_screen = px;

    vec2 ndc = vec2(
        (px.x / frame.resolution.x) * 2.0 - 1.0,
        (px.y / frame.resolution.y) * 2.0 - 1.0
    );
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
}