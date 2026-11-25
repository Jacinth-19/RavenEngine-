// RavenEngine — imported asset runtime representations.
// CPU-side data only; GPU handles live in the renderer (see renderer/GpuResources.h).
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"

namespace raven {

struct Asset {
    UUID uuid;
    AssetType type = AssetType::Unknown;
    virtual ~Asset() = default;
};

struct MipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // RGBA8
};

struct TextureAsset : Asset {
    TextureAsset() { type = AssetType::Texture; }
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t originalWidth = 0;    // pre-import size (for diagnostics)
    uint32_t originalHeight = 0;
    std::vector<MipLevel> mips;    // mip 0 = full res
    bool isSrgb = true;
    bool hasAlpha = false;
};

struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};
static_assert(sizeof(MeshVertex) == 32, "MeshVertex layout is persisted in mesh artifacts");

struct MeshAsset : Asset {
    MeshAsset() { type = AssetType::Mesh; }
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::string name;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    uint32_t triangleCount = 0;
};

struct MaterialAsset : Asset {
    MaterialAsset() { type = AssetType::Material; }
    glm::vec4 albedo{0.8f, 0.8f, 0.8f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.85f;
    glm::vec3 emissive{0.0f};
    float normalScale = 1.0f;
    UUID albedoTexture;            // nil = untextured
    UUID normalTexture;            // nil = untextured
    std::string displayName;
};

enum class ShaderStage : uint32_t { Vertex = 0, Fragment = 1, Compute = 2 };
const char* ShaderStageToString(ShaderStage stage);
ShaderStage ShaderStageFromString(const std::string& text);

struct ShaderAsset : Asset {
    ShaderAsset() { type = AssetType::Shader; }
    std::vector<uint8_t> spirv;    // little-endian SPIR-V words
    ShaderStage stage = ShaderStage::Vertex;
    std::string entryPoint = "main";
    std::string sourcePath;        // origin .glsl (informational)
};

struct AudioAsset : Asset {
    AudioAsset() { type = AssetType::Audio; }
    std::vector<uint8_t> bytes;    // original file bytes (decoded at play time)
    std::string format;            // "wav" | "ogg" | "flac" | "mp3"
};

// Pass-through payloads for file-like assets the engine does not decode yet.
struct RawAsset : Asset {
    std::vector<uint8_t> bytes;
    std::string path;
};

} // namespace raven
