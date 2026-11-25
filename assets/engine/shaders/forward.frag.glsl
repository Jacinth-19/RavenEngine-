#version 460

layout(set = 0, binding = 0) uniform FrameUbo {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj;
    vec4 lightDirIntensity;    // xyz = direction (pointing FROM light), w = intensity
    vec4 lightColorAmbient;    // rgb = light color, w = ambient strength
    vec4 cameraPosTime;        // xyz = camera world pos, w = time
    vec4 pointPos[4];
    vec4 pointColorIntensity[4];
    ivec4 counts;              // x = point light count
} frame;

layout(set = 0, binding = 1) uniform sampler2D shadowMap;
layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 albedo;
    vec4 mrParams; // metallic, roughness, emissiveStrength, shadowEnabled
} push;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vLightSpacePos;

layout(location = 0) out vec4 outColor;

float SampleShadow() {
    if (push.mrParams.w < 0.5) return 1.0;  // shadows disabled for this draw
    vec3 proj = vLightSpacePos.xyz / vLightSpacePos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.x < 0.001 || proj.x > 0.999 || proj.y < 0.001 || proj.y > 0.999 || proj.z > 0.999) return 1.0;
    float bias = 0.0018;
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float depth = texture(shadowMap, proj.xy + vec2(x, y) * texel).r;
            sum += (proj.z - bias) > depth ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    vec4 albedoSample = texture(albedoMap, vUV) * push.albedo;
    float metallic = clamp(push.mrParams.x, 0.0, 1.0);
    float roughness = clamp(push.mrParams.y, 0.04, 1.0);

    vec3 N = normalize(vNormal);
    // Perturb normal with normal map (tangent space approximated via derivatives).
    vec3 normalSample = texture(normalMap, vUV).xyz * 2.0 - 1.0;
    vec3 dPdx = dFdx(vWorldPos);
    vec3 dPdy = dFdy(vWorldPos);
    vec2 dUVdx = dFdx(vUV);
    vec2 dUVdy = dFdy(vUV);
    vec3 T = normalize(dPdx * dUVdy.t - dPdy * dUVdx.t);
    vec3 B = normalize(dPdy * dUVdx.s - dPdx * dUVdy.s);
    vec3 Nmap = normalize(mat3(T, B, N) * normalSample);
    // Blend mapped normal with geometric normal (keeps low-quality normal maps forgiving).
    N = normalize(mix(N, Nmap, 0.75));

    vec3 V = normalize(frame.cameraPosTime.xyz - vWorldPos);
    vec3 L = normalize(-frame.lightDirIntensity.xyz);
    float lightIntensity = frame.lightDirIntensity.w;
    vec3 lightColor = frame.lightColorAmbient.rgb;

    float shadow = SampleShadow();

    // --- diffuse (Lambert) ---
    float ndotl = max(dot(N, L), 0.0);
    vec3 diffuse = albedoSample.rgb * lightColor * (ndotl * lightIntensity * shadow);

    // --- specular (Blinn-Phong, energy-conserving-ish) ---
    vec3 H = normalize(L + V);
    float shininess = mix(256.0, 8.0, roughness * roughness);
    float ndotv = max(dot(N, V), 0.0);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    // Fresnel (Schlick)
    float f0 = mix(0.04, 0.9, metallic);
    float cosTheta = max(dot(H, V), 0.0);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
    vec3 specular = lightColor * (spec * fresnel * lightIntensity * shadow * (1.0 - roughness * 0.7));

    // --- point lights (simple attenuation) ---
    vec3 pointContrib = vec3(0.0);
    int pointCount = min(frame.counts.x, 4);
    for (int i = 0; i < pointCount; ++i) {
        vec3 toLight = frame.pointPos[i].xyz - vWorldPos;
        float distance = length(toLight);
        float radius = max(frame.pointPos[i].w, 0.001);
        if (distance > radius) continue;
        vec3 Lp = toLight / distance;
        float attenuation = pow(clamp(1.0 - distance / radius, 0.0, 1.0), 2.0);
        float ndotlp = max(dot(N, Lp), 0.0);
        vec3 pointColor = frame.pointColorIntensity[i].rgb;
        float pointIntensity = frame.pointColorIntensity[i].w;
        pointContrib += albedoSample.rgb * pointColor * (ndotlp * attenuation * pointIntensity * 0.5);
        vec3 Hp = normalize(Lp + V);
        pointContrib += pointColor * pow(max(dot(N, Hp), 0.0), shininess) * attenuation * pointIntensity * 0.15;
    }

    // --- ambient ---
    float ambientStrength = frame.lightColorAmbient.w;
    vec3 ambient = albedoSample.rgb * ambientStrength;

    // --- metallic workflow lerp: metals keep specular, kill diffuse ---
    vec3 color = mix(diffuse + ambient + pointContrib, specular + ambient * 0.5 + pointContrib, metallic * 0.85)
               + specular + pointContrib * 0.25;

    // --- tonemap-ish soft clip (keeps highlights pleasant without HDR targets) ---
    color = color / (color + vec3(0.6)) * 1.35;

    outColor = vec4(color, albedoSample.a);
}
