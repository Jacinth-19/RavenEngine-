#include "editor/inspectors/MaterialEditor.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <array>
#include <unordered_map>

#include "imgui.h"
#include <nlohmann/json.hpp>

#include "raven/assets/AssetManager.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"
#include "raven/core/UUID.h"

#include "editor/Icons.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/MaterialSnapshotCommand.h"
#include "editor/core/Clipboard.h"

namespace raven::editor {

namespace {

// Per-(path,mtime) parse cache — the material file is tiny, but avoids re-parsing
// JSON every frame and keeps the undo applier consistent with what's on disk.
struct MaterialCache {
    std::filesystem::path path;
    std::filesystem::file_time_type mtime{};
    nlohmann::json json;
    bool loaded = false;
};

MaterialCache& CacheFor(const raven::AssetDatabase::Entry& entry) {
    static std::unordered_map<std::string, MaterialCache> caches;
    const std::string key = entry.uuid.ToString();
    auto it = caches.find(key);
    if (it == caches.end()) it = caches.emplace(key, MaterialCache{}).first;
    MaterialCache& cache = it->second;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(entry.absolutePath, ec);
    if (!cache.loaded || cache.path != entry.absolutePath || cache.mtime != mtime) {
        cache.path = entry.absolutePath;
        cache.mtime = mtime;
        cache.json = nlohmann::json::object();
        cache.loaded = false;
        std::string error;
        if (auto text = raven::fs::ReadText(entry.absolutePath, &error)) {
            nlohmann::json parsed = nlohmann::json::parse(*text, nullptr, false);
            if (parsed.is_object()) { cache.json = std::move(parsed); cache.loaded = true; }
        }
    }
    return cache;
}

void WriteMaterialAndReload(const raven::UUID& uuid, const std::filesystem::path& path, const std::string& text) {
    std::string writeError;
    if (!raven::fs::WriteText(path, text, &writeError)) {
        RAVEN_LOG_ERROR("Material editor: cannot write %s: %s", path.string().c_str(), writeError.c_str());
        return;
    }
    raven::AssetManager::Instance().ReloadAsset(uuid);   // tiny JSON — sync parse is fine
}

// Applies `edit` to the cached JSON; on change, pushes a coalesced snapshot command
// whose applier writes the file and reloads the asset.
void ApplyMaterialEdit(const raven::AssetDatabase::Entry& entry, const char* tag,
                       const std::function<void(nlohmann::json&)>& edit) {
    MaterialCache& cache = CacheFor(entry);
    if (!cache.loaded) return;
    const std::string beforeText = cache.json.dump(2);
    edit(cache.json);
    const std::string afterText = cache.json.dump(2);
    if (beforeText == afterText) return;

    std::filesystem::path path = cache.path;
    const std::string after = afterText;   // command outlives this frame
    const std::string before = beforeText;
    auto applier = [uuid = entry.uuid, path](const std::string& snapshot) {
        WriteMaterialAndReload(uuid, path, snapshot);
    };
    Commands().PushCoalesced(std::string("material:") + tag + ":" + entry.uuid.ToString(),
                             std::make_shared<MaterialSnapshotCommand>(entry.uuid, before, after, applier));
    std::error_code ec;
    cache.mtime = std::filesystem::last_write_time(path, ec);   // applier wrote the file already
}

// Resolves a texture reference that may be a UUID string (canonical .rmat form)
// or a path relative to the assets root. Returns the DB entry or nullptr.
const raven::AssetDatabase::Entry* ResolveTextureRef(const std::string& ref) {
    if (ref.empty()) return nullptr;
    raven::AssetManager& manager = raven::AssetManager::Instance();
    if (!manager.IsInitialized()) return nullptr;
    const raven::AssetDatabase& db = manager.Database();
    if (auto parsed = raven::UUID::Parse(ref)) return db.FindByUUID(*parsed);
    std::string rel = ref;
    if (rel.rfind("assets/", 0) == 0) rel = rel.substr(strlen("assets/"));
    if (const auto* e = db.FindByPath(rel)) return e;
    if (const auto* e = db.FindByPath("assets/" + rel)) return e;
    return nullptr;
}

// Texture-reference row: current value, picker popup over the DB textures.
void DrawTextureRefRow(const char* label, const raven::AssetDatabase::Entry& entry, const char* key) {
    MaterialCache& cache = CacheFor(entry);
    const std::string current = cache.json.value(key, std::string());
    const auto* currentEntry = ResolveTextureRef(current);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(110.0f);
    std::string shown = "—";
    if (currentEntry) shown = std::filesystem::path(currentEntry->relativePath).filename().string();
    else if (!current.empty()) shown = std::filesystem::path(current).filename().string();
    ImGui::TextUnformatted(shown.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", currentEntry ? currentEntry->relativePath.c_str() : (current.empty() ? "not set" : current.c_str()));
    ImGui::SameLine();

    const std::string popupId = std::string("texpick:") + key;
    if (ImGui::SmallButton((std::string("…##texpick:") + key).c_str())) { ImGui::OpenPopup(popupId.c_str()); ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(480.0f, 360.0f)); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick a texture from the project");
    ImGui::SameLine();
    if (ImGui::SmallButton((std::string(ICON_CLOSE) + "##clear" + key).c_str())) {
        ApplyMaterialEdit(entry, key, [&](nlohmann::json& j) { j[key] = ""; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear reference");

    if (ImGui::BeginPopup(popupId.c_str())) {
        // Filter buffers are per-row (the picker is used twice) to avoid cross-talk.
        static std::unordered_map<std::string, std::array<char, 128>> s_Filters;
        std::array<char, 128>& filterBuf = s_Filters[key];
        ImGui::InputTextWithHint("##filter", "filter…", filterBuf.data(), filterBuf.size());
        std::string needle = filterBuf.data();
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        raven::AssetManager& manager = raven::AssetManager::Instance();
        const std::vector<const raven::AssetDatabase::Entry*> textures =
            manager.IsInitialized() ? manager.Database().EntriesOfType(raven::AssetType::Texture)
                                    : std::vector<const raven::AssetDatabase::Entry*>{};
        int shownCount = 0;
        {
            for (const auto* tex : textures) {
                if (!tex) continue;
                std::string lower = tex->relativePath;
                for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!needle.empty() && lower.find(needle) == std::string::npos) continue;
                if (++shownCount > 64) { ImGui::TextDisabled("…"); break; }
                if (ImGui::Selectable(tex->relativePath.c_str())) {
                    const std::string uuidText = tex->uuid.ToString();
                    ApplyMaterialEdit(entry, key, [&](nlohmann::json& j) { j[key] = uuidText; });
                    ImGui::CloseCurrentPopup();
                    break;
                }
            }
        }
        if (textures.empty() || shownCount == 0) ImGui::TextDisabled("(no matching textures)");
        ImGui::EndPopup();
    }
}

} // namespace

void DrawMaterialEditor(const raven::AssetDatabase::Entry& entry) {
    if (entry.type != raven::AssetType::Material) { ImGui::TextDisabled("Not a material asset."); return; }
    MaterialCache& cache = CacheFor(entry);
    if (!cache.loaded) { ImGui::TextDisabled("Cannot parse material JSON."); return; }
    nlohmann::json& json = cache.json;

    auto floatAt = [&](const char* key, size_t index, float fallback) -> float {
        if (!json.contains(key) || !json[key].is_array()) return fallback;
        const auto& arr = json[key];
        if (index >= arr.size() || !arr[index].is_number()) return fallback;
        return arr[index].get<float>();
    };

    ImGui::SeparatorText("Surface");

    float albedo[4] = {floatAt("albedo", 0, 0.8f), floatAt("albedo", 1, 0.8f), floatAt("albedo", 2, 0.8f), floatAt("albedo", 3, 1.0f)};
    if (ImGui::ColorEdit4("Albedo", albedo, ImGuiColorEditFlags_Float)) {
        ApplyMaterialEdit(entry, "albedo", [&](nlohmann::json& j) { j["albedo"] = {albedo[0], albedo[1], albedo[2], albedo[3]}; });
    }
    float emissive[3] = {floatAt("emissive", 0, 0.0f), floatAt("emissive", 1, 0.0f), floatAt("emissive", 2, 0.0f)};
    if (ImGui::ColorEdit3("Emissive", emissive, ImGuiColorEditFlags_Float)) {
        ApplyMaterialEdit(entry, "emissive", [&](nlohmann::json& j) { j["emissive"] = {emissive[0], emissive[1], emissive[2]}; });
    }

    float metallic = json.value("metallic", 0.0f);
    if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f, "%.3f")) {
        ApplyMaterialEdit(entry, "metallic", [&](nlohmann::json& j) { j["metallic"] = metallic; });
    }
    float roughness = json.value("roughness", 0.5f);
    if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.3f")) {
        ApplyMaterialEdit(entry, "roughness", [&](nlohmann::json& j) { j["roughness"] = roughness; });
    }
    float normalScale = json.value("normalScale", 1.0f);
    if (ImGui::SliderFloat("Normal Scale", &normalScale, 0.0f, 2.0f, "%.2f")) {
        ApplyMaterialEdit(entry, "normalScale", [&](nlohmann::json& j) { j["normalScale"] = normalScale; });
    }

    ImGui::SeparatorText("Texture maps");
    DrawTextureRefRow("Albedo Map", entry, "albedoTexture");
    DrawTextureRefRow("Normal Map", entry, "normalTexture");

    ImGui::Separator();
    ImGui::TextDisabled("Edits write %s and reimport in place.", std::filesystem::path(entry.absolutePath).filename().string().c_str());
}

} // namespace raven::editor
