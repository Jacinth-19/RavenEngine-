// RavenEngine — importers. Each importer turns a source file + settings into a
// CPU-side asset and a serialized artifact payload (written to Library/Artifacts).
// DeserializeAsset turns a payload back into the asset on load.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "raven/assets/Asset.h"
#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"

namespace raven {

struct ImportResult {
    bool ok = false;
    std::string error;
    std::shared_ptr<Asset> asset;
    std::vector<uint8_t> payload;   // serialized artifact contents
};

// AssetDatabase forward declaration for texture/material UUID resolution.
class AssetDatabase;

namespace TextureImporter {
    ImportResult Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid);
    std::shared_ptr<TextureAsset> Deserialize(const std::vector<uint8_t>& payload, UUID uuid);
    std::vector<MipLevel> GenerateMipmaps(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);
}

namespace MeshImporter {
    ImportResult Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid);
    std::shared_ptr<MeshAsset> Deserialize(const std::vector<uint8_t>& payload, UUID uuid);
}

namespace MaterialImporter {
    ImportResult Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid,
                        const AssetDatabase* database);
    std::shared_ptr<MaterialAsset> Deserialize(const std::vector<uint8_t>& payload, UUID uuid);
}

namespace ShaderImporter {
    // Source is a precompiled .spv (see scripts/compile_shaders.py). Validates the SPIR-V magic.
    ImportResult Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid);
    std::shared_ptr<ShaderAsset> Deserialize(const std::vector<uint8_t>& payload, UUID uuid);
}

namespace AudioImporter {
    ImportResult Import(const std::filesystem::path& source, const nlohmann::json& settings, UUID uuid);
    std::shared_ptr<AudioAsset> Deserialize(const std::vector<uint8_t>& payload, UUID uuid);
}

// Route by type. Returns ImportResult{ok=false} for unsupported types.
ImportResult ImportAsset(AssetType type, const std::filesystem::path& source,
                         const nlohmann::json& settings, UUID uuid,
                         const AssetDatabase* database);

// Rebuild an asset from a stored artifact payload.
std::shared_ptr<Asset> DeserializeAsset(AssetType type, const std::vector<uint8_t>& payload, UUID uuid);

} // namespace raven
