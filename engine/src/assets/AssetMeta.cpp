#include "raven/assets/AssetMeta.h"
#include "raven/core/FileSystem.h"

namespace raven {

nlohmann::json AssetMeta::ToJson() const {
    nlohmann::json j;
    j["uuid"] = uuid.ToString();
    j["assetType"] = AssetTypeToString(type);
    j["relativePath"] = relativePath;
    j["importVersion"] = importVersion;
    j["importSettings"] = importSettings;
    if (textureRemap.is_object() && !textureRemap.empty()) j["textureRemap"] = textureRemap;
    return j;
}

std::optional<AssetMeta> AssetMeta::FromJson(const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        if (error) *error = "meta root is not an object";
        return std::nullopt;
    }
    auto uuidText = json.value("uuid", std::string());
    auto parsed = UUID::Parse(uuidText);
    if (!parsed) {
        if (error) *error = "meta has missing/invalid uuid";
        return std::nullopt;
    }
    AssetMeta meta;
    meta.uuid = *parsed;
    meta.type = AssetTypeFromString(json.value("assetType", std::string("unknown")));
    meta.relativePath = json.value("relativePath", std::string());
    meta.importVersion = json.value("importVersion", 1u);
    if (json.contains("importSettings") && json["importSettings"].is_object())
        meta.importSettings = json["importSettings"];
    else
        meta.importSettings = nlohmann::json::object();
    if (json.contains("textureRemap") && json["textureRemap"].is_object())
        meta.textureRemap = json["textureRemap"];
    return meta;
}

std::filesystem::path MetaPathFor(const std::filesystem::path& sourceAsset) {
    auto p = sourceAsset;
    p += ".meta";
    return p;
}

std::optional<AssetMeta> ReadMeta(const std::filesystem::path& metaPath, std::string* error) {
    auto text = fs::ReadText(metaPath, error);
    if (!text) return std::nullopt;
    nlohmann::json json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded()) {
        if (error) *error = "meta file is not valid JSON: " + metaPath.string();
        return std::nullopt;
    }
    return AssetMeta::FromJson(json, error);
}

bool WriteMeta(const std::filesystem::path& metaPath, const AssetMeta& meta, std::string* error) {
    return fs::WriteText(metaPath, meta.ToJson().dump(2), error);
}

nlohmann::json DefaultImportSettings(AssetType type) {
    switch (type) {
        case AssetType::Texture:
            return {{"srgb", true}, {"generateMipmaps", true}, {"maxSize", 0}};  // maxSize 0 = no clamp
        case AssetType::Mesh:
            return {{"flipUVs", true}, {"flipWinding", false}};
        case AssetType::Shader:
            return {{"stage", "vertex"}, {"entry", "main"}};
        case AssetType::Audio:
            return {{"force3D", false}};
        default:
            return nlohmann::json::object();
    }
}

} // namespace raven
