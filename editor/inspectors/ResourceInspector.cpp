#include "editor/inspectors/ResourceInspector.h"

#include <filesystem>
#include <string>

#include "imgui.h"

#include "raven/assets/AssetDatabase.h"
#include "raven/assets/AssetManager.h"
#include "raven/core/Logger.h"

#include "editor/Icons.h"
#include "editor/core/Clipboard.h"
#include "editor/inspectors/MaterialEditor.h"
#include "editor/inspectors/TextureEditor.h"
#include "editor/selection/EditorSelection.h"

namespace raven::editor {

namespace {

void DrawAssetInfoBlock(const raven::AssetDatabase::Entry& entry) {
    const std::filesystem::path abs = entry.absolutePath;
    ImGui::TextDisabled("Path");
    ImGui::SameLine(90.0f);
    ImGui::TextUnformatted(entry.relativePath.c_str());
    ImGui::TextDisabled("UUID");
    ImGui::SameLine(90.0f);
    ImGui::TextUnformatted(entry.uuid.ToString().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_COPY "##copyUuid")) CopyTextToClipboard(entry.uuid.ToString());
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::exists(abs, ec) ? std::filesystem::file_size(abs, ec) : 0;
    ImGui::TextDisabled("Size");
    ImGui::SameLine(90.0f);
    ImGui::Text("%llu bytes", static_cast<unsigned long long>(bytes));
}

const char* AssetTypeGlyph(raven::AssetType type) {
    switch (type) {
        case raven::AssetType::Material: return ICON_ASSET_MATERIAL;
        case raven::AssetType::Texture:  return ICON_ASSET_IMAGE;
        case raven::AssetType::Mesh:     return ICON_ASSET_MESH;
        default:                         return ICON_PACKAGE;
    }
}

} // namespace

void ResourceInspector::Draw(const raven::UUID& asset) {
    raven::AssetManager& manager = raven::AssetManager::Instance();
    if (!manager.IsInitialized()) {
        ImGui::TextDisabled("Asset database unavailable.");
        return;
    }
    const raven::AssetDatabase::Entry* entry = manager.Database().FindByUUID(asset);
    if (!entry) {
        ImGui::TextDisabled("Asset %s is not in the database (rescan needed).", asset.ToString().c_str());
        return;
    }

    // Header: glyph + name + kind badge, same visual language as the entity inspector.
    const std::string displayName = std::filesystem::path(entry->relativePath).stem().string();
    ImGui::Text("%s  %s", AssetTypeGlyph(entry->type), displayName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", raven::AssetTypeToString(entry->type));
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_COPY "##copyPath")) CopyTextToClipboard(entry->relativePath);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy asset path");
    ImGui::SameLine();
    if (ImGui::SmallButton("entity mode")) ClearAssetSelection();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the entity inspector (clears the asset selection)");
    ImGui::Separator();

    switch (entry->type) {
        case raven::AssetType::Material: DrawMaterialEditor(*entry); break;
        case raven::AssetType::Texture:  DrawTextureEditor(*entry);  break;
        default:
            DrawAssetInfoBlock(*entry);
            ImGui::Separator();
            ImGui::TextDisabled("A dedicated inspector for this type lands in a later phase.");
            break;
    }
}

} // namespace raven::editor
