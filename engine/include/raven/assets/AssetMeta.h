// RavenEngine — .meta sidecar files: stable UUID + import settings per source asset.
// Format (JSON): { "uuid": "...", "assetType": "texture", "relativePath": "assets/x.png",
//                  "importVersion": 1, "importSettings": { ... } }
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"

namespace raven {

struct AssetMeta {
    UUID uuid;
    AssetType type = AssetType::Unknown;
    std::string relativePath;              // informational; authoritative mapping lives in the DB
    uint32_t importVersion = 1;
    nlohmann::json importSettings = nlohmann::json::object();
    // §10.4: model material-slot -> texture (assets-root-relative path) mapping,
    // persisted across sessions. Written by the TextureRemapDialog; consumed by the
    // smart model import (assimp). Empty object = never asked.
    nlohmann::json textureRemap = nlohmann::json::object();

    nlohmann::json ToJson() const;
    static std::optional<AssetMeta> FromJson(const nlohmann::json& json, std::string* error = nullptr);
};

std::filesystem::path MetaPathFor(const std::filesystem::path& sourceAsset);
std::optional<AssetMeta> ReadMeta(const std::filesystem::path& metaPath, std::string* error = nullptr);
bool WriteMeta(const std::filesystem::path& metaPath, const AssetMeta& meta, std::string* error = nullptr);
nlohmann::json DefaultImportSettings(AssetType type);

} // namespace raven
