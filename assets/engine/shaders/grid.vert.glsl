#version 460
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(push_constant) uniform PushConstants { mat4 viewProj; } push;
layout(location = 0) out vec3 vColor;
void main() {
    vColor = inColor;
    gl_Position = push.viewProj * vec4(inPosition, 1.0);
}
