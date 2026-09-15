#version 450
// Fullscreen triangle from gl_VertexIndex — no vertex buffer needed.
void main() {
    vec2 pos = vec2(float((gl_VertexIndex & 1) << 1), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
