#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform SkyUBO {
    mat4 invViewProj;      // 反投影矩阵（屏幕 → 世界）
    vec4 sunDir;           // xyz = 太阳方向（归一化），w = 太阳强度
    vec4 camPos;           // xyz = 相机世界位置，w = 保留
} sky;

const float PI = 3.14159265359;

float rayleighPhase(float cosTheta) {
    return 0.75 * (1.0 + cosTheta * cosTheta);
}

float miePhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (3.0 / (8.0 * PI)) * ((1.0 - g2) * (1.0 + cosTheta * cosTheta))
           / (denom * sqrt(max(denom, 1e-6)));
}

void main() {
    vec4 clip = vec4(fragUV * 2.0 - 1.0, 1.0, 1.0);
    vec4 world = sky.invViewProj * clip;
    vec3 farPoint = world.xyz / world.w;
    vec3 rayDir = normalize(farPoint - sky.camPos.xyz);

    vec3 sun = normalize(sky.sunDir.xyz);
    float cosTheta = clamp(dot(rayDir, sun), -1.0, 1.0);
    float cosView = clamp(rayDir.y, -1.0, 1.0);

    float pr = rayleighPhase(cosTheta);
    float pm = miePhase(cosTheta, 0.76);

    float dr = exp(-max(rayDir.y, 0.0) * 8.0);
    float dm = exp(-max(rayDir.y, 0.0) * 1.2);
    float horizon = pow(1.0 - abs(rayDir.y), 3.0);

    vec3 rayleigh = vec3(5.5, 13.0, 22.4);
    float mie = 21.0;
    float darkMode = sky.camPos.w;
    float I = sky.sunDir.w * (0.15 + sun.y);

    vec3 col;
    col.r = rayleigh.x * pr * (dr + horizon * 0.5) + mie * pm * dm;
    col.g = rayleigh.y * pr * (dr + horizon * 0.5) + mie * pm * dm;
    col.b = rayleigh.z * pr * (dr + horizon * 0.5) + mie * pm * dm;

    col *= I * 0.0015;

    float brightness = mix(1.0, 0.35, darkMode);
    vec3 tint = mix(vec3(1.0), vec3(0.55, 0.68, 1.05), darkMode);
    col *= brightness * tint;

    float sunDisk = pow(max(dot(rayDir, sun), 0.0), 512.0);
    col += vec3(1.0, 0.95, 0.85) * sunDisk * mix(4.0, 1.6, darkMode);

    col = 1.0 - exp(-col);
    outColor = vec4(col, 1.0);
}
