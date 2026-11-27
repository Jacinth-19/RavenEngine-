#include "raven/assets/AssetTypes.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace raven {

const char* AssetTypeToString(AssetType type) {
    switch (type) {
        case AssetType::Texture:    return "texture";
        case AssetType::Mesh:       return "mesh";
        case AssetType::Material:   return "material";
        case AssetType::Shader:     return "shader";
        case AssetType::Audio:      return "audio";
        case AssetType::Scene:      return "scene";
        case AssetType::LuaScript:  return "luaScript";
        case AssetType::Font:       return "font";
        case AssetType::Prefab:     return "prefab";
        case AssetType::Anim:       return "anim";
        default:                    return "unknown";
    }
}

AssetType AssetTypeFromString(const std::string& text) {
    std::string s = text;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "texture") return AssetType::Texture;
    if (s == "mesh") return AssetType::Mesh;
    if (s == "material") return AssetType::Material;
    if (s == "shader") return AssetType::Shader;
    if (s == "audio") return AssetType::Audio;
    if (s == "scene") return AssetType::Scene;
    if (s == "luascript") return AssetType::LuaScript;
    if (s == "font") return AssetType::Font;
    if (s == "prefab") return AssetType::Prefab;
    if (s == "anim") return AssetType::Anim;
    return AssetType::Unknown;
}

AssetType AssetTypeFromExtension(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Double extensions first (e.g. ".frag.spv" handled via the compound check below).
    std::string compound = p.filename().string();
    std::transform(compound.begin(), compound.end(), compound.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto endsWith = [&](const std::string& suffix) {
        return compound.size() >= suffix.size() &&
               compound.compare(compound.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (endsWith(".frag.spv") || endsWith(".vert.spv") || endsWith(".comp.spv") || endsWith(".tesc.spv") || endsWith(".tese.spv") || endsWith(".spv"))
        return AssetType::Shader;

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".psd")
        return AssetType::Texture;
    if (ext == ".obj")  // glTF/FBX planned (see docs/ROADMAP.md)
        return AssetType::Mesh;
    if (ext == ".rmat" || ext == ".rmaterial" || ext == ".ravenmaterial")
        return AssetType::Material;
    if (ext == ".ravenshader")
        return AssetType::Shader;
    if (ext == ".ravenanim")
        return AssetType::Anim;
    if (ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3")
        return AssetType::Audio;
    if (ext == ".ravenscene")
        return AssetType::Scene;
    if (ext == ".lua")
        return AssetType::LuaScript;
    if (ext == ".ttf" || ext == ".otf")
        return AssetType::Font;
    if (ext == ".ravenprefab")
        return AssetType::Prefab;
    return AssetType::Unknown;
}

} // namespace raven
