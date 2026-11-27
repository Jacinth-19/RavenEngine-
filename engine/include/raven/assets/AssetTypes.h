// RavenEngine — asset type taxonomy + extension mapping.
#pragma once

#include <cstdint>
#include <string>

namespace raven {

enum class AssetType : uint32_t {
    Unknown = 0,
    Texture,
    Mesh,
    Material,
    Shader,
    Audio,
    Scene,
    LuaScript,
    Font,
    Prefab,
    Anim,          // skeletal animation clips (.ravenanim)
};

const char* AssetTypeToString(AssetType type);
AssetType AssetTypeFromString(const std::string& text);
AssetType AssetTypeFromExtension(const std::string& path);  // uses file extension

} // namespace raven
