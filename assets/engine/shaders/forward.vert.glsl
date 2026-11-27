#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform FrameUbo {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj;
    vec4 lightDirIntensity;
    vec4 lightColorAmbient;
    vec4 cameraPosTime;
    vec4 pointPos[4];
    vec4 pointColorIntensity[4];
    ivec4 counts;
} frame;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 albedo;
    vec4 mrParams; // metallic, roughness, emissiveStrength, unused
} push;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vLightSpacePos;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = normalize(mat3(transpose(inverse(push.model))) * inNormal);
    vUV = inUV;
    vLightSpacePos = frame.lightViewProj * worldPos;
    gl_Position = frame.proj * frame.view * worldPos;
}
