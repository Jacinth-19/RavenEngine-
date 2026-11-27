#include "editor/panels/ProjectPanel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "raven/assets/AssetDatabase.h"
#include "raven/assets/AssetManager.h"
#include "raven/core/Logger.h"

#include "editor/Icons.h"
#include "editor/core/AssetDragDrop.h"
#include "editor/core/AssetOperations.h"
#include "editor/core/Clipboard.h"
#include "editor/widgets/ThumbnailCache.h"
#include "editor/widgets/ImportProgress.h"
#include "editor/widgets/TextureRemapDialog.h"
#include "editor/selection/EditorSelection.h"
#include "editor/Icons.h"

namespace raven::editor {

namespace {

std::string ToForwardSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char* TypeGlyph(AssetType type, bool isDirectory) {
    if (isDirectory) return ICON_ASSET_FOLDER;
    switch (type) {
        case AssetType::Texture:   return ICON_ASSET_IMAGE;
        case AssetType::Mesh:      return ICON_ASSET_MESH;
        case AssetType::Material:  return ICON_ASSET_MATERIAL;
        case AssetType::Shader:    return ICON_ASSET_SHADER;
        case AssetType::Audio:     return ICON_ASSET_AUDIO;
        case AssetType::Scene:     return ICON_ASSET_SCENE;
        case AssetType::LuaScript: return ICON_ASSET_SCRIPT;
        case AssetType::Font:      return ICON_TEXT;
        case AssetType::Prefab:    return ICON_ASSET_PREFAB;
        case AssetType::Anim:      return ICON_ASSET_ANIM;
        default:                   return ICON_ASSET_GENERIC;
    }
}

std::string ExtBadge(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext = Lower(name.substr(dot + 1));
    if (ext.size() > 5) ext = ext.substr(0, 5);
    return ext;
}

std::string FormatSize(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

} // namespace

ProjectPanel::ProjectPanel(Config config) : m_Config(std::move(config)) {
    m_Expanded.insert("");   // root conceptually always open
}

void ProjectPanel::RescanAndRefresh() {
    if (raven::AssetManager::Instance().IsInitialized()) {
        std::string error;
        if (!raven::AssetManager::Instance().Database().Rescan(&error))
            RAVEN_LOG_WARN("Assets: rescan failed: %s", error.c_str());
    }
    if (m_Config.afterFileOpFn) m_Config.afterFileOpFn();
    m_Dirty = true;
}

void ProjectPanel::Rebuild() {
    m_Entries.clear();
    m_Folders.clear();
    m_LastScan = ImGui::GetTime();
    m_Dirty = false;

    const std::filesystem::path root = m_Config.assetsDirFn ? m_Config.assetsDirFn() : std::filesystem::path();
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return;

    const raven::AssetDatabase* db = nullptr;
    if (raven::AssetManager::Instance().IsInitialized()) db = &raven::AssetManager::Instance().Database();

    std::error_code walkEc;
    for (const auto& it : std::filesystem::recursive_directory_iterator(root, walkEc)) {
        const std::filesystem::path abs = it.path();
        std::error_code relEc;
        const std::filesystem::path rel = std::filesystem::relative(abs, root, relEc);
        if (relEc) continue;
        const std::string relStr = ToForwardSlashes(rel.string());
        if (relStr.empty() || relStr.rfind("..", 0) == 0) continue;

        const std::string filename = abs.filename().string();
        if (filename.size() >= 5 && filename.compare(filename.size() - 5, 5, ".meta") == 0) continue;   // sidecars

        if (it.is_directory()) {
            m_Folders.insert(relStr);
            Entry e;
            e.relPath = relStr;
            e.name = filename;
            e.absolute = abs;
            e.isDirectory = true;
            m_Entries.push_back(std::move(e));
            continue;
        }

        Entry e;
        e.relPath = relStr;
        e.name = filename;
        e.absolute = abs;
        e.sizeBytes = std::filesystem::file_size(abs, ec);
        if (ec) ec.clear();
        if (db) {
            if (const raven::AssetDatabase::Entry* dbEntry = db->FindByPath(relStr)) {
                e.type = dbEntry->type;
                e.uuid = dbEntry->uuid;
                e.isDirty = dbEntry->isDirty;
            }
        }
        if (e.type == raven::AssetType::Unknown) e.type = raven::AssetTypeFromExtension(e.name);
        m_Entries.push_back(std::move(e));
    }

    std::sort(m_Entries.begin(), m_Entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return Lower(a.name) < Lower(b.name);
    });

    // Drop selection/rename state for paths that vanished.
    for (auto it = m_Selected.begin(); it != m_Selected.end();) {
        const bool exists = FindByRel(*it) != nullptr;
        it = exists ? std::next(it) : m_Selected.erase(it);
    }
    if (!m_Primary.empty() && !m_Selected.count(m_Primary)) m_Primary = m_Selected.empty() ? std::string() : *m_Selected.begin();
    if (!m_Renaming.empty() && !FindByRel(m_Renaming)) m_Renaming.clear();

    // Resource-mode: drop the asset selection when the asset left the DB (file
    // deleted/renamed under us). `db == nullptr` means the manager isn't up yet —
    // leave the selection alone in that case.
    if (db && editor::HasAssetSelection() && !db->FindByUUID(editor::SelectedAsset()))
        editor::ClearAssetSelection();
}

const ProjectPanel::Entry* ProjectPanel::FindByRel(const std::string& relPath) const {
    for (const Entry& e : m_Entries) if (e.relPath == relPath) return &e;
    return nullptr;
}

std::vector<const ProjectPanel::Entry*> ProjectPanel::VisibleEntries() const {
    const std::string filter = Lower(m_FilterBuf);
    std::vector<const Entry*> shown;
    if (!filter.empty()) {
        // Search: flattened recursive results, substring over the relative path.
        for (const Entry& e : m_Entries) {
            if (e.isDirectory) continue;
            if (Lower(e.relPath).find(filter) != std::string::npos) shown.push_back(&e);
        }
        return shown;
    }
    const std::string prefix = m_CurrentFolder.empty() ? std::string() : m_CurrentFolder + "/";
    for (const Entry& e : m_Entries) {
        if (e.isDirectory) {
            const size_t slash = e.relPath.rfind('/');
            const bool directChild = m_CurrentFolder.empty() ? (slash == std::string::npos)
                                                             : (slash != std::string::npos && e.relPath.substr(0, slash) == m_CurrentFolder);
            if (directChild) shown.push_back(&e);
            continue;
        }
        if (e.relPath.rfind(prefix, 0) != 0) continue;                 // outside current folder
        if (e.relPath.find('/', prefix.size()) != std::string::npos) continue;   // nested — not a direct child
        shown.push_back(&e);
    }
    return shown;
}

std::vector<std::string> ProjectPanel::ChildFolders(const std::string& relFolder) const {
    std::vector<std::string> out;
    const std::string prefix = relFolder.empty() ? std::string() : relFolder + "/";
    for (const std::string& folder : m_Folders) {
        if (folder == relFolder || folder.rfind(prefix, 0) != 0) continue;
        if (folder.find('/', prefix.size()) == std::string::npos) out.push_back(folder);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return out;
}

void ProjectPanel::SyncAssetSelection() {
    // Resource-mode inspector follows the primary selection; folders and
    // not-in-DB entries clear it (entity inspector takes over, §6.3).
    const Entry* primary = m_Primary.empty() ? nullptr : FindByRel(m_Primary);
    if (primary && !primary->isDirectory && !primary->uuid.IsNil()) {
        editor::SelectAsset(primary->uuid);
    } else {
        editor::ClearAssetSelection();
    }
}

void ProjectPanel::ClickSelect(const Entry& entry) {
    if (ImGui::GetIO().KeyCtrl) {
        if (IsSelected(entry.relPath)) {
            m_Selected.erase(entry.relPath);
            if (m_Primary == entry.relPath) m_Primary = m_Selected.empty() ? std::string() : *m_Selected.begin();
        } else {
            m_Selected.insert(entry.relPath);
            m_Primary = entry.relPath;
        }
        SyncAssetSelection();
        return;
    }
    m_Selected.clear();
    m_Selected.insert(entry.relPath);
    m_Primary = entry.relPath;
    SyncAssetSelection();
}

void ProjectPanel::Draw() {
    if (!ImGui::Begin("Assets", m_Config.openFlag)) {
        ImGui::End();
        if (m_Config.focusSinkFn) m_Config.focusSinkFn(false);
        return;
    }
    const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_Config.focusSinkFn) m_Config.focusSinkFn(panelFocused);

    const double now = ImGui::GetTime();
    const float rescanSeconds = m_Config.rescanSecondsFn ? m_Config.rescanSecondsFn() : 2.0f;
    if (m_Dirty || (now - m_LastScan) > static_cast<double>(rescanSeconds)) Rebuild();

    DrawToolbar();

    // Search bar; Esc clears.
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##assetSearch", ICON_SEARCH " Search assets…", m_FilterBuf, sizeof(m_FilterBuf));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && m_FilterBuf[0] != '\0') m_FilterBuf[0] = '\0';
    ImGui::Separator();

    // F2 starts an inline rename of the primary selection (only while the panel has focus).
    if (ImGui::IsKeyPressed(ImGuiKey_F2) && panelFocused && !m_Primary.empty() && m_Renaming.empty() && !ImGui::GetIO().WantTextInput)
        if (const Entry* e = FindByRel(m_Primary)) BeginInlineRename(*e);
    // Ctrl+D duplicates the selection (the app yields its entity Ctrl+D while we hold focus).
    if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !m_Selected.empty()) {
        for (const std::string& rel : m_Selected)
            if (const Entry* e = FindByRel(rel)) DuplicateAsset(e->absolute);
        RescanAndRefresh();
    }
    // Delete key queues the selected items for the confirm modal (never for the scene's selection).
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_Selected.empty() && !ImGui::GetIO().WantTextInput && panelFocused) {
        m_DeleteQueue.clear();
        for (const std::string& rel : m_Selected)
            if (const Entry* e = FindByRel(rel)) m_DeleteQueue.push_back(e->absolute.string());
        ImGui::OpenPopup("assetDeleteConfirm");
    }

    if (m_FilterBuf[0] == '\0') DrawCrumbBar();

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("assetSplit", ImVec2(0.0f, -footerHeight), false);

    // Left pane — folder tree.
    ImGui::BeginChild("folderTree", ImVec2(200.0f, 0.0f), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.65f, 0.72f, 1.0f));
    ImGui::TextUnformatted("FOLDERS");
    ImGui::PopStyleColor();
    DrawEntryTreeNode("");
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane — grid or list.
    ImGui::BeginChild("assetEntries", ImVec2(0.0f, 0.0f), false);
    const std::vector<const Entry*> shown = VisibleEntries();
    if (shown.empty()) {
        ImGui::TextDisabled(m_FilterBuf[0] != '\0' ? "No assets match — Esc clears the search." : "Folder is empty.");
    } else if (m_ListView) {
        DrawEntryList(shown);
    } else {
        DrawEntryGrid(shown);
    }
    DrawContextMenus(nullptr);   // background popup for the entries region
    ImGui::EndChild();

    ImGui::EndChild();   // assetSplit

    DrawEntryFooter();
    DrawDeleteModal();

    ImGui::End();
}

void ProjectPanel::DrawToolbar() {
    if (ImGui::SmallButton(ICON_REFRESH "##refreshAssets")) RescanAndRefresh();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan assets (mints .meta sidecars for new files)");
    ImGui::SameLine();

    // View-mode toggle pair.
    const auto viewButton = [this](const char* icon, bool listMode, const char* tooltip) {
        const bool active = m_ListView == listMode;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(icon)) m_ListView = listMode;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
    };
    viewButton(ICON_PANEL_GRID, false, "Grid view");
    viewButton(ICON_LIST, true, "List view");

    // Collapse / expand all folders in the tree.
    if (ImGui::SmallButton(ICON_COLLAPSE_ALL "##collapseFolders")) { m_Expanded.clear(); ++m_TreeEpoch; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse all folders");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_EXPAND_ALL "##expandFolders")) {
        for (const std::string& folder : m_Folders) m_Expanded.insert(folder);
        ++m_TreeEpoch;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand all folders");
    ImGui::SameLine();

    const size_t assetCount = std::count_if(m_Entries.begin(), m_Entries.end(), [](const Entry& e) { return !e.isDirectory; });
    ImGui::TextDisabled("%zu asset(s)", assetCount);
}

void ProjectPanel::DrawCrumbBar() {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));

    if (ImGui::SmallButton("assets")) m_CurrentFolder.clear();
    size_t offset = 0;
    std::string accumulated;
    while (offset < m_CurrentFolder.size()) {
        const size_t slash = m_CurrentFolder.find('/', offset);
        const size_t end = (slash == std::string::npos) ? m_CurrentFolder.size() : slash;
        const std::string part = m_CurrentFolder.substr(offset, end - offset);
        accumulated = accumulated.empty() ? part : accumulated + "/" + part;
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        const std::string target = accumulated;
        if (ImGui::SmallButton(part.c_str())) m_CurrentFolder = target;
        offset = end + 1;
    }
    ImGui::PopStyleColor(3);
    ImGui::Separator();
}

void ProjectPanel::DrawEntryTreeNode(const std::string& relFolder) {
    const bool isRoot = relFolder.empty();
    if (isRoot) {
        const bool selected = m_CurrentFolder.empty();
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(1.0f, 0.85f, 0.4f, 1.0f) : ImVec4(0.85f, 0.87f, 0.91f, 1.0f));
        if (ImGui::Selectable(ICON_ASSET_FOLDER "  assets", selected)) m_CurrentFolder.clear();
        ImGui::PopStyleColor();
    }

    for (const std::string& folder : ChildFolders(relFolder)) {
        const std::string name = folder.substr(folder.find_last_of('/') + 1);
        const bool expanded = m_Expanded.count(folder) != 0;
        const bool selected = m_CurrentFolder == folder;
        const std::vector<std::string> sub = ChildFolders(folder);
        const bool hasSub = !sub.empty();

        ImGui::PushID((std::to_string(m_TreeEpoch) + ":" + folder).c_str());
        ImGui::Indent(12.0f);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (!hasSub) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (expanded) flags |= ImGuiTreeNodeFlags_DefaultOpen;   // seeds fresh nodes; storage owns state after
        const std::string label = std::string(expanded ? ICON_FOLDER_OPEN : ICON_ASSET_FOLDER) + "  " + name;
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_CurrentFolder = folder;
        // Persist the tree's open state (Collapse/Expand-all drive m_Expanded).
        if (open) m_Expanded.insert(folder); else m_Expanded.erase(folder);
        if (open && hasSub) {
            DrawEntryTreeNode(folder);
            ImGui::TreePop();
        }
        ImGui::Unindent(12.0f);
        ImGui::PopID();
    }
}

void ProjectPanel::DrawEntryGrid(const std::vector<const Entry*>& shown) {
    const float cell = 88.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>(width / cell));

    ImGui::Columns(cols, "assetGrid", false);
    for (const Entry* entry : shown) {
        ImGui::PushID(entry->relPath.c_str());
        const bool selected = IsSelected(entry->relPath);
        const bool renaming = m_Renaming == entry->relPath;

        const ImVec2 cellSize(cell - 6.0f, cell - 4.0f);
        const bool clicked = ImGui::InvisibleButton("cell", cellSize);
        if (clicked) ClickSelect(*entry);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry->isDirectory) m_CurrentFolder = entry->relPath;
            else if (entry->absolute.extension() == ".ravenscene" && m_Config.openSceneFn) m_Config.openSceneFn(entry->absolute);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        if (selected || hovered) {
            dl->AddRectFilled(min, max, selected ? IM_COL32(90, 120, 190, 90) : IM_COL32(255, 255, 255, 14), 6.0f);
            if (selected) dl->AddRect(min, max, IM_COL32(140, 175, 240, 200), 6.0f, 0, 1.5f);
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            if (!entry->isDirectory && !entry->uuid.IsNil()) {
                const AssetUuidPayload payload = AssetUuidPayload::Make(entry->uuid, entry->type);
                ImGui::SetDragDropPayload(kAssetUuidPayloadType, &payload, sizeof(payload));
                ImGui::TextUnformatted(entry->name.c_str());
            }
            ImGui::EndDragDropSource();
        }

        // Thumbnail (texture entries only) or type glyph (spec §10.2: Ready paints the
        // image, Pending paints a placeholder spinner, Failed falls back to the glyph).
        const char* glyph = TypeGlyph(entry->type, entry->isDirectory);
        const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
        bool drewContent = false;
        if (!entry->isDirectory && entry->type == raven::AssetType::Texture && !entry->uuid.IsNil()) {
            ImTextureID tex{};
            float aspect = 1.0f;
            const ThumbnailState tstate = ThumbnailCache::Instance().Get(entry->uuid, entry->absolute, entry->type, &tex, &aspect);
            const float areaTop = min.y + 6.0f;
            const float areaBottom = max.y - 22.0f;
            const float areaH = areaBottom - areaTop;
            const float areaW = cellSize.x - 10.0f;
            if (tstate == ThumbnailState::Ready && tex != ImTextureID_Invalid) {
                float imgH = areaH;
                float imgW = aspect > 0.0f ? imgH * aspect : imgH;
                if (imgW > areaW) { imgW = areaW; imgH = aspect > 0.0f ? imgW / aspect : imgW; }
                const ImVec2 imgMin(min.x + (cellSize.x - imgW) * 0.5f, areaTop + (areaH - imgH) * 0.5f);
                dl->AddImage(ImTextureRef(tex), imgMin, ImVec2(imgMin.x + imgW, imgMin.y + imgH));
                drewContent = true;
            } else if (tstate == ThumbnailState::Pending) {
                // Placeholder: dim glyph + rotating arc (ImGui has no spinner widget).
                dl->AddText(ImVec2(min.x + (cellSize.x - glyphSize.x) * 0.5f, areaTop + 2.0f), IM_COL32(140, 146, 162, 220), glyph);
                const ImVec2 center(min.x + cellSize.x * 0.5f, areaTop + areaH * 0.55f);
                const float radius = 9.0f;
                const float start = static_cast<float>(ImGui::GetTime()) * 3.6f;
                dl->PathArcTo(center, radius, start, start + 4.2f, 16);
                dl->PathStroke(IM_COL32(150, 175, 240, 255), 0, 2.5f);
                drewContent = true;
            } else if (tstate == ThumbnailState::Failed) {
                // §10.2: generic type icon with error tint — never blocks the grid.
                dl->AddText(ImVec2(min.x + (cellSize.x - glyphSize.x) * 0.5f, areaTop + 2.0f), IM_COL32(228, 116, 108, 255), glyph);
                drewContent = true;
            }
        }
        if (!drewContent)
            dl->AddText(ImVec2(min.x + (cellSize.x - glyphSize.x) * 0.5f, min.y + 12.0f), IM_COL32(222, 228, 240, 255), glyph);

        if (renaming) {
            ImGui::SetCursorScreenPos(ImVec2(min.x + 4.0f, max.y - 22.0f));
            DrawRenameEditor(*entry, cellSize.x - 8.0f);
        } else {
            std::string label = entry->name;
            while (label.size() > 4 && ImGui::CalcTextSize(label.c_str()).x > cellSize.x - 8.0f)
                label = label.substr(0, label.size() - 4) + "…";
            const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(min.x + (cellSize.x - labelSize.x) * 0.5f, max.y - 19.0f), IM_COL32(200, 204, 214, 255), label.c_str());
        }

        const std::string badge = entry->isDirectory ? std::string() : ExtBadge(entry->name);
        if (!badge.empty()) {
            const ImVec2 badgeSize = ImGui::CalcTextSize(badge.c_str());
            dl->AddText(ImVec2(max.x - badgeSize.x - 4.0f, min.y + 3.0f), IM_COL32(150, 158, 175, 255), badge.c_str());
        }
        if (entry->isDirty) dl->AddCircleFilled(ImVec2(max.x - 8.0f, max.y - 8.0f), 3.0f, IM_COL32(255, 200, 90, 255));

        if (hovered) ImGui::SetTooltip("%s\n%s", entry->relPath.c_str(),
                                       entry->uuid.IsNil() ? "(unregistered)" : entry->uuid.ToString().c_str());
        DrawContextMenus(entry);
        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
}

void ProjectPanel::DrawEntryList(const std::vector<const Entry*>& shown) {
    if (!ImGui::BeginTable("assetTable", 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 0.0f)))
        return;
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry* entry = shown[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(entry->relPath.c_str());
            const bool selected = IsSelected(entry->relPath);
            const bool renaming = m_Renaming == entry->relPath;
            if (renaming) {
                DrawRenameEditor(*entry, ImGui::GetContentRegionAvail().x);
            } else {
                const std::string label = std::string(TypeGlyph(entry->type, entry->isDirectory)) + "  " + entry->name;
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    ClickSelect(*entry);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (entry->isDirectory) m_CurrentFolder = entry->relPath;
                        else if (entry->absolute.extension() == ".ravenscene" && m_Config.openSceneFn)
                            m_Config.openSceneFn(entry->absolute);
                    }
                }
                if (ImGui::BeginDragDropSource()) {
                    if (!entry->isDirectory && !entry->uuid.IsNil()) {
                        const AssetUuidPayload payload = AssetUuidPayload::Make(entry->uuid, entry->type);
                        ImGui::SetDragDropPayload(kAssetUuidPayloadType, &payload, sizeof(payload));
                        ImGui::TextUnformatted(entry->name.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
                DrawContextMenus(entry);
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", entry->isDirectory ? "folder" : AssetTypeToString(entry->type));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", entry->isDirectory ? "" : FormatSize(entry->sizeBytes).c_str());
            ImGui::TableNextColumn();
            if (!entry->uuid.IsNil()) ImGui::TextDisabled("%.8s…", entry->uuid.ToString().c_str());
            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::EndTable();
}

void ProjectPanel::BeginInlineRename(const Entry& entry) {
    m_Renaming = entry.relPath;
    std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", entry.name.c_str());
}

void ProjectPanel::DrawEntryFooter() {
    if (m_Selected.empty()) {
        ImGui::TextDisabled("%s", m_CurrentFolder.empty() ? "assets/" : ("assets/" + m_CurrentFolder).c_str());
        return;
    }
    ImGui::TextDisabled("%zu selected", m_Selected.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reimport")) {
        int queued = 0;
        for (const std::string& rel : m_Selected) {
            if (const Entry* e = FindByRel(rel); e && !e->uuid.IsNil() && !e->isDirectory) {
                editor::ImportProgress::Instance().EnqueueReload(e->name, e->uuid);
                ++queued;
            }
        }
        if (queued > 0) RAVEN_LOG_INFO("Assets: queued %d asset(s) for reimport", queued);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-import from source and re-upload to the GPU (async)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy UUIDs")) {
        std::string joined;
        for (const std::string& rel : m_Selected) {
            if (const Entry* e = FindByRel(rel); e && !e->uuid.IsNil()) {
                if (!joined.empty()) joined += '\n';
                joined += e->uuid.ToString();
            }
        }
        CopyTextToClipboard(joined);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the selected UUIDs, one per line");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { m_Selected.clear(); m_Primary.clear(); editor::ClearAssetSelection(); }
}

void ProjectPanel::DrawContextMenus(const Entry* item) {
    // Background (right-click an empty area of the entries pane).
    if (!item) {
        if (ImGui::BeginPopupContextWindow("assetBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            const std::filesystem::path root = m_Config.assetsDirFn ? m_Config.assetsDirFn() : std::filesystem::path();
            const std::filesystem::path currentAbs = m_CurrentFolder.empty() ? root : root / m_CurrentFolder;
            if (ImGui::MenuItem(ICON_FOLDER_NEW " New Folder")) {
                m_FilterBuf[0] = '\0';   // reveal the new item for the rename editor
                const AssetOpResult result = CreateFolder(currentAbs, "New Folder");
                if (result.ok) {
                    RescanAndRefresh();
                    std::error_code ec;
                    m_Renaming = ToForwardSlashes(std::filesystem::relative(result.path, root, ec).string());
                    std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", result.path.filename().string().c_str());
                }
            }
            if (ImGui::MenuItem(ICON_ASSET_MATERIAL " New Material")) {
                m_FilterBuf[0] = '\0';   // reveal the new item for the rename editor
                const AssetOpResult result = CreateMaterialAsset(currentAbs, "New Material.rmat");
                if (result.ok) {
                    RescanAndRefresh();
                    std::error_code ec;
                    m_Renaming = ToForwardSlashes(std::filesystem::relative(result.path, root, ec).string());
                    std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", result.path.filename().string().c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_REFRESH " Refresh")) RescanAndRefresh();
            if (ImGui::MenuItem(ICON_REVEAL " Reveal in File Manager")) RevealInFileManager(currentAbs);
            ImGui::EndPopup();
        }
        return;
    }

    // Item menu (right-click a cell/row). Right-clicking an unselected item selects it.
    if (!ImGui::BeginPopupContextItem("assetItem")) return;
    if (!IsSelected(item->relPath)) ClickSelect(*item);

    const bool isScene = item->absolute.extension() == ".ravenscene";
    if (ImGui::MenuItem(isScene ? "Open Scene" : "Open…")) {
        if (isScene && m_Config.openSceneFn) m_Config.openSceneFn(item->absolute);
        else RAVEN_LOG_INFO("Assets: open requested for %s", item->relPath.c_str());
    }
    if (item->type == raven::AssetType::Mesh && ImGui::MenuItem(ICON_ASSET_MESH " Remap Textures…"))
        raven::editor::TextureRemapDialog::Instance().Open(item->absolute, item->uuid, item->name);
    if (ImGui::MenuItem(ICON_RENAME " Rename", "F2")) BeginInlineRename(*item);
    if (ImGui::MenuItem(ICON_COPY " Duplicate", "Ctrl+D")) {
        for (const std::string& rel : m_Selected)
            if (const Entry* e = FindByRel(rel)) DuplicateAsset(e->absolute);
        RescanAndRefresh();
    }
    if (ImGui::MenuItem(ICON_DELETE " Delete", "Del")) {
        m_DeleteQueue.clear();
        for (const std::string& rel : m_Selected)
            if (const Entry* e = FindByRel(rel)) m_DeleteQueue.push_back(e->absolute.string());
        ImGui::OpenPopup("assetDeleteConfirm");
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_LINK " Copy Path")) CopyTextToClipboard(item->relPath);
    if (ImGui::MenuItem(ICON_TEXT " Copy UUID", nullptr, false, !item->uuid.IsNil()))
        CopyTextToClipboard(item->uuid.ToString());
    ImGui::EndPopup();
}

void ProjectPanel::DrawDeleteModal() {
    if (m_DeleteQueue.empty()) return;
    if (!ImGui::IsPopupOpen("assetDeleteConfirm")) ImGui::OpenPopup("assetDeleteConfirm");
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("assetDeleteConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped(ICON_LOG_WARN "  Permanently delete %zu item(s)? This cannot be undone.", m_DeleteQueue.size());
    ImGui::Spacing();
    if (ImGui::Button(ICON_DELETE " Delete", ImVec2(120.0f, 0.0f))) {
        int deleted = 0;
        for (const std::string& abs : m_DeleteQueue)
            if (DeleteAsset(std::filesystem::path(abs)).ok) ++deleted;
        RAVEN_LOG_INFO("Assets: deleted %d item(s)", deleted);
        m_DeleteQueue.clear();
        m_Selected.clear();
        m_Primary.clear();
        editor::ClearAssetSelection();
        RescanAndRefresh();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        m_DeleteQueue.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProjectPanel::DrawRenameEditor(const Entry& entry, float width) {
    ImGui::PushItemWidth(width);
    ImGui::SetKeyboardFocusHere();
    const bool enterPressed = ImGui::InputText("##renameAsset", m_RenameBuf, sizeof(m_RenameBuf),
                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool focusLost = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopItemWidth();
    if (!enterPressed && !focusLost) return;

    const std::string newName = (m_RenameBuf[0] != '\0') ? std::string(m_RenameBuf) : entry.name;
    const AssetOpResult result = RenameAsset(entry.absolute, newName);
    if (!result.ok) RAVEN_LOG_WARN("Assets: rename failed: %s", result.error.c_str());
    m_Renaming.clear();
    if (result.ok) {
        RescanAndRefresh();
        if (m_Config.assetsDirFn) {
            std::error_code ec;
            std::string rel = ToForwardSlashes(std::filesystem::relative(result.path, m_Config.assetsDirFn(), ec).string());
            m_Selected.clear();
            m_Selected.insert(rel);
            m_Primary = rel;
            SyncAssetSelection();   // UUID survives the rename — re-resolve
        }
    }
}

} // namespace raven::editor
