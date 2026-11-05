#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inColor;
layout(push_constant) uniform PushBlock {
    mat4 mvp;
    mat4 model;
} push;

layout(binding = 0) uniform UBO {
    vec3 lightDir;
    float lightIntensity;
    vec3 camPos;
    float _pad0;
    vec3 baseColor;
    float metallic;
    float roughness;
    float useTexture;
} ubo;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec3 fragColor;

void main() {
    vec4 worldPos = push.model * vec4(inPos, 1.0);
    gl_Position = push.mvp * vec4(inPos, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = mat3(push.model) * inNormal;
    fragUV = inUV;
    fragColor = inColor * ubo.baseColor;
}
