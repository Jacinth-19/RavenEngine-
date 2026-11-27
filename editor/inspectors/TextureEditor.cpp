#include "editor/inspectors/TextureEditor.h"

#include <filesystem>
#include <functional>
#include <string>

#include "imgui.h"
#include <stb_image.h>
#include <nlohmann/json.hpp>

#include "raven/assets/AssetManager.h"
#include "raven/assets/AssetMeta.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"
#include "raven/core/UUID.h"

#include "editor/Icons.h"
#include "editor/commands/AssetMetaSnapshotCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/widgets/ImportProgress.h"

namespace raven::editor {

namespace {

// Cheap header-only dimension probe; DDS/BCn return false → "unknown".
bool ProbeDimensions(const std::filesystem::path& path, int* w, int* h, int* channels) {
    return stbi_info(path.string().c_str(), w, h, channels) != 0;
}

std::string DisplayNameFor(const raven::AssetDatabase::Entry& entry) {
    return std::filesystem::path(entry.relativePath).filename().string();
}

// Applies `edit` to the sidecar's importSettings, pushes a coalesced snapshot
// command (one undo entry per burst), and the applier writes the sidecar back
// and enqueues an async reimport.
void CommitImportSettings(const raven::AssetDatabase::Entry& entry,
                          const std::function<void(nlohmann::json&)>& edit) {
    const std::filesystem::path metaPath = raven::MetaPathFor(entry.absolutePath);
    std::string err;
    auto meta = raven::ReadMeta(metaPath, &err);
    if (!meta) {
        RAVEN_LOG_ERROR("Texture editor: cannot read %s: %s", metaPath.string().c_str(), err.c_str());
        return;
    }
    const std::string before = meta->importSettings.dump();
    edit(meta->importSettings);
    const std::string after = meta->importSettings.dump();
    if (before == after) return;

    const std::string label = DisplayNameFor(entry);
    const raven::UUID uuid = entry.uuid;
    auto applier = [metaPath, label, uuid](const std::string& snapshot) {
        auto current = raven::ReadMeta(metaPath, nullptr);
        if (!current) {
            RAVEN_LOG_ERROR("Texture editor: cannot re-read %s for undo/redo", metaPath.string().c_str());
            return;
        }
        nlohmann::json parsed = nlohmann::json::parse(snapshot, nullptr, false);
        current->importSettings = parsed.is_discarded() ? nlohmann::json::object() : std::move(parsed);
        std::string writeErr;
        if (!raven::WriteMeta(metaPath, *current, &writeErr)) {
            RAVEN_LOG_ERROR("Texture editor: cannot write %s: %s", metaPath.string().c_str(), writeErr.c_str());
            return;
        }
        ImportProgress::Instance().EnqueueReload(label, uuid);
    };
    Commands().PushCoalesced("importSettings:" + uuid.ToString(),
                             std::make_shared<AssetMetaSnapshotCommand>(uuid, before, after, applier));
}

} // namespace

void DrawTextureEditor(const raven::AssetDatabase::Entry& entry) {
    if (entry.type != raven::AssetType::Texture) { ImGui::TextDisabled("Not a texture asset."); return; }

    ImGui::SeparatorText("File");
    int w = 0, h = 0, channels = 0;
    const bool probed = ProbeDimensions(entry.absolutePath, &w, &h, &channels);
    ImGui::TextDisabled("Dimensions");
    ImGui::SameLine(110.0f);
    if (probed) ImGui::Text("%d x %d", w, h);
    else ImGui::TextUnformatted("unknown (unsupported container)");
    ImGui::TextDisabled("Channels");
    ImGui::SameLine(110.0f);
    if (probed) ImGui::Text("%d", channels);
    else ImGui::TextUnformatted("?");

    std::error_code ec;
    const uintmax_t bytes = std::filesystem::exists(entry.absolutePath, ec) ? std::filesystem::file_size(entry.absolutePath, ec) : 0;
    ImGui::TextDisabled("Size on disk");
    ImGui::SameLine(110.0f);
    ImGui::Text("%llu bytes", static_cast<unsigned long long>(bytes));

    ImGui::SeparatorText("Import settings");

    auto meta = raven::ReadMeta(raven::MetaPathFor(entry.absolutePath), nullptr);
    if (!meta) { ImGui::TextDisabled("(no .meta sidecar — rescan the project)"); return; }
    const nlohmann::json settings = meta->importSettings;

    bool srgb = settings.value("srgb", true);
    if (ImGui::Checkbox("sRGB", &srgb)) {
        CommitImportSettings(entry, [&](nlohmann::json& s) { s["srgb"] = srgb; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Treat as color data (false for normal/roughness/metalness maps)");

    bool genMips = settings.value("generateMipmaps", true);
    if (ImGui::Checkbox("Generate mipmaps", &genMips)) {
        CommitImportSettings(entry, [&](nlohmann::json& s) { s["generateMipmaps"] = genMips; });
    }

    int maxSize = settings.value("maxSize", 0);
    if (ImGui::InputInt("Max size (0 = no clamp)", &maxSize)) {
        if (maxSize < 0) maxSize = 0;
        CommitImportSettings(entry, [&](nlohmann::json& s) { s["maxSize"] = maxSize; });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stored in the .meta; the importer does not resize yet (imported 1:1)");

    bool compress = settings.value("compress", false);
    if (ImGui::Checkbox("BCn compress on import", &compress)) {
        CommitImportSettings(entry, [&](nlohmann::json& s) { s["compress"] = compress; });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stored in the .meta; the BCn artifact path (.raven/cache/bc7) lands with the renderer's block-upload support");

    ImGui::Separator();
    if (ImGui::Button(ICON_REFRESH " Reimport now")) {
        ImportProgress::Instance().EnqueueReload(DisplayNameFor(entry), entry.uuid);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Settings apply on the next import; reimport is queued async.");
}

} // namespace raven::editor
