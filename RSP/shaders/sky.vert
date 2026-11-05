#version 450

layout(location = 0) out vec2 fragUV;

void main() {
    // 全屏三角形（3 顶点，无需顶点缓冲，gl_VertexIndex = 0/1/2）
    // pos 范围是 {0,2}，映射到 NDC 需 *2-1；fragUV 必须映射到 [0,1] 供片元反投影。
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    fragUV = pos * 0.5;
}
