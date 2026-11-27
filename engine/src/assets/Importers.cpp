#include <utility>
#include "raven/assets/Importers.h"

#include <stb_image.h>
#include <tiny_obj_loader.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "raven/assets/Artifact.h"
#include "raven/assets/AssetDatabase.h"
#include "raven/assets/BcnCodec.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"

namespace raven {

static UUID JsonToUUIDOrNil(const std::string& text) {
    if (text.empty()) return UUID::Nil();
    auto parsed = UUID::Parse(text);
    return parsed.value_or(UUID::Nil());
}

// ================================================================ Texture

std::vector<MipLevel> TextureImporter::GenerateMipmaps(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    std::vector<MipLevel> mips;
    MipLevel base;
    base.width = width;
    base.height = height;
    base.pixels = rgba;
    mips.push_back(std::move(base));

    while (width > 1 || height > 1) {
        const MipLevel& src = mips.back();
        uint32_t dstW = std::max(1u, src.width / 2);
        uint32_t dstH = std::max(1u, src.height / 2);
        MipLevel dst;
        dst.width = dstW;
        dst.height = dstH;
        dst.pixels.resize(static_cast<size_t>(dstW) * dstH * 4);
        for (uint32_t y = 0; y < dstH; ++y) {
            for (uint32_t x = 0; x < dstW; ++x) {
                uint32_t sx = std::min(x * 2, src.width - 1);
                uint32_t sy = std::min(y * 2, src.height - 1);
                uint32_t sx1 = std::min(sx + 1, src.width - 1);
                uint32_t sy1 = std::min(sy + 1, src.height - 1);
                for (int c = 0; c < 4; ++c) {
                    uint32_t a = src.pixels[(sy * src.width + sx) * 4 + c];
                    uint32_t b = src.pixels[(sy * src.width + sx1) * 4 + c];
                    uint32_t d = src.pixels[(sy1 * src.width + sx) * 4 + c];
                    uint32_t e = src.pixels[(sy1 * src.width + sx1) * 4 + c];
                    dst.pixels[(y * dstW + x) * 4 + c] = static_cast<uint8_t>((a + b + d + e + 2) / 4);
                }
            }
        }
        mips.push_back(std::move(dst));
        width = dstW;
        height = dstH;
    }
    return mips;
}

ImportResult TextureImporter::Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid) {
    ImportResult result;
    int w = 0, h = 0, channels = 0;
    stbi_set_flip_vertically_on_load(0);
    stbi_uc* pixels = stbi_load(source.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        result.error = std::string("stbi_load failed: ") + stbi_failure_reason();
        return result;
    }
    std::vector<uint8_t> rgba(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);

    bool hasAlpha = false;
    if (channels == 4 || channels == 2) {
        for (size_t i = 3; i < rgba.size(); i += 4) {
            if (rgba[i] != 255) { hasAlpha = true; break; }
        }
    }

    bool generateMips = settings.value("generateMipmaps", true);
    auto asset = std::make_shared<TextureAsset>();
    asset->uuid = uuid;
    asset->width = static_cast<uint32_t>(w);
    asset->height = static_cast<uint32_t>(h);
    asset->originalWidth = asset->width;
    asset->originalHeight = asset->height;
    asset->isSrgb = settings.value("srgb", true);
    asset->hasAlpha = hasAlpha;
    asset->mips = generateMips ? GenerateMipmaps(rgba, asset->width, asset->height)
                               : std::vector<MipLevel>{MipLevel{asset->width, asset->height, std::move(rgba)}};

    // Import-time BCn (§16 Phase 3): encode every mip to BC blocks and persist
    // alongside the artifact. Best-effort — failure only logs; the RGBA .rasset
    // remains authoritative until the renderer's compressed-format path lands.
    if (const auto bcFormat = BcnFormatFromSetting(settings)) {
        const std::string cacheDir = settings.value("bc7CacheDir", std::string());
        if (!cacheDir.empty()) {
            std::vector<std::pair<uint32_t, uint32_t>> mipDims;
            std::vector<std::vector<uint8_t>> mipBlocks;
            mipDims.reserve(asset->mips.size());
            mipBlocks.reserve(asset->mips.size());
            size_t totalBlockBytes = 0;
            for (const auto& mip : asset->mips) {
                std::vector<uint8_t> blocks = EncodeBcnBlocks(*bcFormat, mip.pixels.data(), mip.width, mip.height);
                if (blocks.empty()) {
                    RAVEN_LOG_WARN("TextureImporter: BCn encode failed for a mip of %s — skipping .bc7 artifact", source.string().c_str());
                    mipBlocks.clear();
                    break;
                }
                totalBlockBytes += blocks.size();
                mipDims.emplace_back(mip.width, mip.height);
                mipBlocks.push_back(std::move(blocks));
            }
            if (!mipBlocks.empty()) {
                std::string bcError;
                const std::filesystem::path out = std::filesystem::path(cacheDir) / (uuid.ToString() + ".bc7");
                if (WriteBcnFile(out, *bcFormat, mipDims, mipBlocks, &bcError)) {
                    RAVEN_LOG_INFO("TextureImporter: wrote %s artifact (%s, %zu mips, %zu bytes) for %s",
                                   out.string().c_str(), BcnFormatToString(*bcFormat), mipBlocks.size(),
                                   totalBlockBytes, source.filename().string().c_str());
                } else {
                    RAVEN_LOG_WARN("TextureImporter: could not write %s: %s", out.string().c_str(), bcError.c_str());
                }
            }
        } else {
            RAVEN_LOG_WARN("TextureImporter: 'compress' set but no bc7CacheDir in settings — BCn skipped for %s", source.filename().string().c_str());
        }
    }

    // Payload: "TEX1" | u32 mipCount | per mip: u32 w, u32 h, u64 bytes | pixel bytes...
    PayloadWriter writer;
    writer.WriteBytes("TEX1", 4);
    writer.WriteU32(static_cast<uint32_t>(asset->mips.size()));
    writer.WriteU32(asset->isSrgb ? 1u : 0u);
    writer.WriteU32(asset->hasAlpha ? 1u : 0u);
    for (const auto& mip : asset->mips) {
        writer.WriteU32(mip.width);
        writer.WriteU32(mip.height);
        writer.WriteU64(mip.pixels.size());
    }
    for (const auto& mip : asset->mips) writer.WriteBytes(mip.pixels.data(), mip.pixels.size());

    result.ok = true;
    result.asset = std::move(asset);
    result.payload = writer.Data();
    return result;
}

std::shared_ptr<TextureAsset> TextureImporter::Deserialize(const std::vector<uint8_t>& payload, UUID uuid) {
    PayloadReader reader(payload.data(), payload.size());
    char magic[4];
    if (!reader.ReadBytes(magic, 4) || std::memcmp(magic, "TEX1", 4) != 0) return nullptr;
    uint32_t mipCount = 0, srgb = 1, hasAlpha = 0;
    if (!reader.ReadU32(mipCount) || !reader.ReadU32(srgb) || !reader.ReadU32(hasAlpha)) return nullptr;
    if (mipCount == 0 || mipCount > 32) return nullptr;

    struct MipHeader { uint32_t w, h; uint64_t bytes; };
    std::vector<MipHeader> headers(mipCount);
    for (auto& hdr : headers) {
        if (!reader.ReadU32(hdr.w) || !reader.ReadU32(hdr.h) || !reader.ReadU64(hdr.bytes)) return nullptr;
        if (hdr.bytes != static_cast<uint64_t>(hdr.w) * hdr.h * 4) return nullptr;
    }
    auto asset = std::make_shared<TextureAsset>();
    asset->uuid = uuid;
    asset->isSrgb = srgb != 0;
    asset->hasAlpha = hasAlpha != 0;
    asset->mips.resize(mipCount);
    for (uint32_t i = 0; i < mipCount; ++i) {
        asset->mips[i].width = headers[i].w;
        asset->mips[i].height = headers[i].h;
        asset->mips[i].pixels.resize(static_cast<size_t>(headers[i].bytes));
        if (!reader.ReadBytes(asset->mips[i].pixels.data(), asset->mips[i].pixels.size())) return nullptr;
    }
    asset->width = asset->mips[0].width;
    asset->height = asset->mips[0].height;
    asset->originalWidth = asset->width;
    asset->originalHeight = asset->height;
    return asset;
}

// ================================================================ Mesh

ImportResult MeshImporter::Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid) {
    ImportResult result;
    bool flipUVs = settings.value("flipUVs", true);

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, source.string().c_str(), nullptr, true)) {
        result.error = "tinyobjloader: " + (err.empty() ? warn : err);
        return result;
    }

    auto asset = std::make_shared<MeshAsset>();
    asset->uuid = uuid;
    asset->name = source.stem().string();

    // Dedup identical (position, normal, uv) corner triples.
    struct Key {
        int pi, ni, ti;
        bool operator==(const Key& o) const { return pi == o.pi && ni == o.ni && ti == o.ti; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<int>{}(k.pi);
            h ^= std::hash<int>{}(k.ni) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(k.ti) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<Key, uint32_t, KeyHash> dedup;
    asset->vertices.reserve(attrib.vertices.size() / 3 / 2 + 16);
    asset->indices.reserve(attrib.vertices.size());

    glm::vec3 boundsMin(1e30f), boundsMax(-1e30f);

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            int fv = shape.mesh.num_face_vertices[f];  // 3 after triangulation
            for (int v = 0; v < fv; ++v) {
                const tinyobj::index_t& idx = shape.mesh.indices[indexOffset + v];
                Key key{idx.vertex_index, idx.normal_index, idx.texcoord_index};
                auto it = dedup.find(key);
                if (it != dedup.end()) {
                    asset->indices.push_back(it->second);
                    continue;
                }
                MeshVertex vertex{};
                vertex.position = {attrib.vertices[3 * idx.vertex_index + 0],
                                   attrib.vertices[3 * idx.vertex_index + 1],
                                   attrib.vertices[3 * idx.vertex_index + 2]};
                if (idx.normal_index >= 0) {
                    vertex.normal = {attrib.normals[3 * idx.normal_index + 0],
                                     attrib.normals[3 * idx.normal_index + 1],
                                     attrib.normals[3 * idx.normal_index + 2]};
                } else {
                    vertex.normal = {0, 1, 0};
                }
                if (idx.texcoord_index >= 0) {
                    float u = attrib.texcoords[2 * idx.texcoord_index + 0];
                    float vcoord = attrib.texcoords[2 * idx.texcoord_index + 1];
                    if (flipUVs) vcoord = 1.0f - vcoord;
                    vertex.uv = {u, vcoord};
                } else {
                    vertex.uv = {0, 0};
                }
                boundsMin = glm::min(boundsMin, vertex.position);
                boundsMax = glm::max(boundsMax, vertex.position);
                uint32_t newIndex = static_cast<uint32_t>(asset->vertices.size());
                asset->vertices.push_back(vertex);
                dedup.emplace(key, newIndex);
                asset->indices.push_back(newIndex);
            }
            indexOffset += static_cast<size_t>(fv);
        }
    }

    if (asset->vertices.empty() || asset->indices.empty()) {
        result.error = "OBJ produced no geometry: " + source.string();
        return result;
    }
    asset->triangleCount = static_cast<uint32_t>(asset->indices.size() / 3);
    asset->boundsMin = boundsMin;
    asset->boundsMax = boundsMax;

    // Payload: "MSH1" | u32 vertexCount | u32 indexCount | u32 stride(=32) | vertices | indices
    PayloadWriter writer;
    writer.WriteBytes("MSH1", 4);
    writer.WriteU32(static_cast<uint32_t>(asset->vertices.size()));
    writer.WriteU32(static_cast<uint32_t>(asset->indices.size()));
    writer.WriteU32(sizeof(MeshVertex));
    writer.WriteBytes(asset->vertices.data(), asset->vertices.size() * sizeof(MeshVertex));
    writer.WriteBytes(asset->indices.data(), asset->indices.size() * sizeof(uint32_t));
    writer.WriteBytes(&asset->boundsMin, sizeof(glm::vec3));
    writer.WriteBytes(&asset->boundsMax, sizeof(glm::vec3));
    writer.WriteU32(asset->triangleCount);

    result.ok = true;
    result.asset = std::move(asset);
    result.payload = writer.Data();
    return result;
}

std::shared_ptr<MeshAsset> MeshImporter::Deserialize(const std::vector<uint8_t>& payload, UUID uuid) {
    PayloadReader reader(payload.data(), payload.size());
    char magic[4];
    if (!reader.ReadBytes(magic, 4) || std::memcmp(magic, "MSH1", 4) != 0) return nullptr;
    uint32_t vertexCount = 0, indexCount = 0, stride = 0;
    if (!reader.ReadU32(vertexCount) || !reader.ReadU32(indexCount) || !reader.ReadU32(stride)) return nullptr;
    if (stride != sizeof(MeshVertex)) return nullptr;
    if (!reader.BytesLeft(static_cast<size_t>(vertexCount) * stride + static_cast<size_t>(indexCount) * 4 + sizeof(glm::vec3) * 2 + 4))
        return nullptr;

    auto asset = std::make_shared<MeshAsset>();
    asset->uuid = uuid;
    asset->vertices.resize(vertexCount);
    asset->indices.resize(indexCount);
    if (!reader.ReadBytes(asset->vertices.data(), vertexCount * sizeof(MeshVertex))) return nullptr;
    if (!reader.ReadBytes(asset->indices.data(), indexCount * sizeof(uint32_t))) return nullptr;
    reader.ReadBytes(&asset->boundsMin, sizeof(glm::vec3));
    reader.ReadBytes(&asset->boundsMax, sizeof(glm::vec3));
    reader.ReadU32(asset->triangleCount);
    return asset;
}

// ================================================================ Material

ImportResult MaterialImporter::Import(const std::filesystem::path& source, const nlohmann::json& /*settings*/, UUID uuid,
                                      const AssetDatabase* database) {
    ImportResult result;
    auto text = fs::ReadText(source, &result.error);
    if (!text) return result;
    nlohmann::json json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded()) {
        result.error = "material is not valid JSON: " + source.string();
        return result;
    }

    auto asset = std::make_shared<MaterialAsset>();
    asset->uuid = uuid;
    asset->displayName = source.stem().string();

    auto resolveTexture = [&](const char* key) -> UUID {
        if (!json.contains(key) || !json[key].is_string()) return UUID::Nil();
        std::string ref = json[key].get<std::string>();
        if (ref.empty()) return UUID::Nil();
        // Allow direct UUID strings...
        if (auto parsed = UUID::Parse(ref)) return *parsed;
        // ...or a path relative to the assets root.
        if (database) {
            // Accept both "assets/x.png" and root-relative "x.png".
            std::string rel = ref;
            if (rel.rfind("assets/", 0) == 0) rel = rel.substr(strlen("assets/"));
            if (const auto* entry = database->FindByPath(rel)) return entry->uuid;
            if (const auto* entry = database->FindByPath("assets/" + rel)) return entry->uuid;
            RAVEN_LOG_WARN("MaterialImporter: texture reference '%s' not found in asset database", ref.c_str());
        }
        return UUID::Nil();
    };

    if (json.contains("albedo") && json["albedo"].is_array() && json["albedo"].size() >= 3) {
        const auto& a = json["albedo"];
        asset->albedo = glm::vec4(a[0].get<float>(), a[1].get<float>(), a[2].get<float>(),
                                  a.size() >= 4 ? a[3].get<float>() : 1.0f);
    }
    asset->metallic = json.value("metallic", 0.0f);
    asset->roughness = json.value("roughness", 0.85f);
    asset->normalScale = json.value("normalScale", 1.0f);
    if (json.contains("emissive") && json["emissive"].is_array() && json["emissive"].size() >= 3) {
        asset->emissive = glm::vec3(json["emissive"][0].get<float>(), json["emissive"][1].get<float>(), json["emissive"][2].get<float>());
    }
    asset->albedoTexture = resolveTexture("albedoTexture");
    asset->normalTexture = resolveTexture("normalTexture");

    // Payload = canonical JSON of the resolved asset.
    nlohmann::json out;
    out["displayName"] = asset->displayName;
    out["albedo"] = {asset->albedo.r, asset->albedo.g, asset->albedo.b, asset->albedo.a};
    out["metallic"] = asset->metallic;
    out["roughness"] = asset->roughness;
    out["normalScale"] = asset->normalScale;
    out["emissive"] = {asset->emissive.r, asset->emissive.g, asset->emissive.b};
    out["albedoTexture"] = asset->albedoTexture.IsNil() ? std::string() : asset->albedoTexture.ToString();
    out["normalTexture"] = asset->normalTexture.IsNil() ? std::string() : asset->normalTexture.ToString();
    std::string payloadText = out.dump();

    result.ok = true;
    result.asset = std::move(asset);
    result.payload.assign(payloadText.begin(), payloadText.end());
    return result;
}

std::shared_ptr<MaterialAsset> MaterialImporter::Deserialize(const std::vector<uint8_t>& payload, UUID uuid) {
    nlohmann::json json = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
    if (json.is_discarded()) return nullptr;
    auto asset = std::make_shared<MaterialAsset>();
    asset->uuid = uuid;
    asset->displayName = json.value("displayName", std::string());
    if (json.contains("albedo") && json["albedo"].is_array() && json["albedo"].size() >= 4)
        asset->albedo = glm::vec4(json["albedo"][0], json["albedo"][1], json["albedo"][2], json["albedo"][3]);
    asset->metallic = json.value("metallic", 0.0f);
    asset->roughness = json.value("roughness", 0.85f);
    asset->normalScale = json.value("normalScale", 1.0f);
    if (json.contains("emissive") && json["emissive"].is_array() && json["emissive"].size() >= 3)
        asset->emissive = glm::vec3(json["emissive"][0], json["emissive"][1], json["emissive"][2]);
    asset->albedoTexture = JsonToUUIDOrNil(json.value("albedoTexture", std::string()));
    asset->normalTexture = JsonToUUIDOrNil(json.value("normalTexture", std::string()));
    return asset;
}

// ================================================================ Shader

ImportResult ShaderImporter::Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid) {
    ImportResult result;
    auto bytes = fs::ReadBinary(source, &result.error);
    if (!bytes) return result;
    if (bytes->size() < 5 || (*bytes)[0] != 0x03 || (*bytes)[1] != 0x02 || (*bytes)[2] != 0x23 || (*bytes)[3] != 0x07) {
        result.error = "file is not SPIR-V (expected magic 0x07230203): " + source.string();
        return result;
    }
    auto asset = std::make_shared<ShaderAsset>();
    asset->uuid = uuid;
    asset->spirv = *bytes;
    asset->stage = ShaderStageFromString(settings.value("stage", std::string("vertex")));
    asset->entryPoint = settings.value("entry", std::string("main"));
    asset->sourcePath = source.string();

    // Payload: "SPV1" | u32 stage | u32 entryLen | entry bytes | spv bytes
    PayloadWriter writer;
    writer.WriteBytes("SPV1", 4);
    writer.WriteU32(static_cast<uint32_t>(asset->stage));
    writer.WriteU32(static_cast<uint32_t>(asset->entryPoint.size()));
    writer.WriteBytes(asset->entryPoint.data(), asset->entryPoint.size());
    writer.WriteBytes(asset->spirv.data(), asset->spirv.size());

    result.ok = true;
    result.asset = std::move(asset);
    result.payload = writer.Data();
    return result;
}

std::shared_ptr<ShaderAsset> ShaderImporter::Deserialize(const std::vector<uint8_t>& payload, UUID uuid) {
    PayloadReader reader(payload.data(), payload.size());
    char magic[4];
    if (!reader.ReadBytes(magic, 4) || std::memcmp(magic, "SPV1", 4) != 0) return nullptr;
    uint32_t stage = 0, entryLen = 0;
    if (!reader.ReadU32(stage) || !reader.ReadU32(entryLen)) return nullptr;
    if (entryLen > 256) return nullptr;
    std::string entry(entryLen, '\0');
    if (entryLen && !reader.ReadBytes(entry.data(), entryLen)) return nullptr;
    size_t spvOffset = payload.size() - 0;  // rest of buffer
    size_t consumed = 4 + 4 + 4 + entryLen;
    (void)spvOffset;
    if (payload.size() < consumed) return nullptr;
    auto asset = std::make_shared<ShaderAsset>();
    asset->uuid = uuid;
    asset->stage = static_cast<ShaderStage>(stage);
    asset->entryPoint = entry.empty() ? "main" : entry;
    asset->spirv.assign(payload.begin() + static_cast<std::ptrdiff_t>(consumed), payload.end());
    return asset;
}

// ================================================================ Audio

ImportResult AudioImporter::Import(const std::filesystem::path& source, const nlohmann::json& /*settings*/, UUID uuid) {
    ImportResult result;
    auto bytes = fs::ReadBinary(source, &result.error);
    if (!bytes) return result;

    std::string format;
    if (bytes->size() >= 12 && std::memcmp(bytes->data(), "RIFF", 4) == 0) format = "wav";
    else if (bytes->size() >= 4 && std::memcmp(bytes->data(), "OggS", 4) == 0) format = "ogg";
    else if (bytes->size() >= 4 && std::memcmp(bytes->data(), "fLaC", 4) == 0) format = "flac";
    else if (bytes->size() >= 3 && std::memcmp(bytes->data(), "ID3", 3) == 0) format = "mp3";
    else if (bytes->size() >= 2 && (*bytes)[0] == 0xFF && ((*bytes)[1] & 0xE0) == 0xE0) format = "mp3";
    else {
        result.error = "unrecognized audio container (expected WAV/OGG/FLAC/MP3): " + source.string();
        return result;
    }

    auto asset = std::make_shared<AudioAsset>();
    asset->uuid = uuid;
    asset->bytes = *bytes;
    asset->format = format;

    PayloadWriter writer;
    writer.WriteBytes("AUD1", 4);
    writer.WriteU32(static_cast<uint32_t>(format.size()));
    writer.WriteBytes(format.data(), format.size());
    writer.WriteBytes(asset->bytes.data(), asset->bytes.size());

    result.ok = true;
    result.asset = std::move(asset);
    result.payload = writer.Data();
    return result;
}

std::shared_ptr<AudioAsset> AudioImporter::Deserialize(const std::vector<uint8_t>& payload, UUID uuid) {
    PayloadReader reader(payload.data(), payload.size());
    char magic[4];
    if (!reader.ReadBytes(magic, 4) || std::memcmp(magic, "AUD1", 4) != 0) return nullptr;
    uint32_t formatLen = 0;
    if (!reader.ReadU32(formatLen) || formatLen > 16) return nullptr;
    std::string format(formatLen, '\0');
    if (formatLen && !reader.ReadBytes(format.data(), formatLen)) return nullptr;
    size_t consumed = 4 + 4 + formatLen;
    auto asset = std::make_shared<AudioAsset>();
    asset->uuid = uuid;
    asset->format = format;
    asset->bytes.assign(payload.begin() + static_cast<std::ptrdiff_t>(consumed), payload.end());
    return asset;
}

// ================================================================ routing

ImportResult ImportAsset(AssetType type, const std::filesystem::path& source,
                         const nlohmann::json& settings, UUID uuid,
                         const AssetDatabase* database) {
    switch (type) {
        case AssetType::Texture:  return TextureImporter::Import(source, settings, uuid);
        case AssetType::Mesh:     return MeshImporter::Import(source, settings, uuid);
        case AssetType::Material: return MaterialImporter::Import(source, settings, uuid, database);
        case AssetType::Shader:   return ShaderImporter::Import(source, settings, uuid);
        case AssetType::Audio:    return AudioImporter::Import(source, settings, uuid);
        default: {
            ImportResult result;
            result.error = std::string("no importer for asset type '") + AssetTypeToString(type) + "'";
            return result;
        }
    }
}

std::shared_ptr<Asset> DeserializeAsset(AssetType type, const std::vector<uint8_t>& payload, UUID uuid) {
    switch (type) {
        case AssetType::Texture:  return TextureImporter::Deserialize(payload, uuid);
        case AssetType::Mesh:     return MeshImporter::Deserialize(payload, uuid);
        case AssetType::Material: return MaterialImporter::Deserialize(payload, uuid);
        case AssetType::Shader:   return ShaderImporter::Deserialize(payload, uuid);
        case AssetType::Audio:    return AudioImporter::Deserialize(payload, uuid);
        default: return nullptr;
    }
}

} // namespace raven
