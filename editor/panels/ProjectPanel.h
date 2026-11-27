// RavenEditor — ProjectPanel (§10.1): the asset browser.
//
// Left pane: recursive folder tree (dirs only, semantic icons, current folder
// highlighted). Right pane: grid of entries (type glyph, ext badge) or a details
// list, toggleable from the toolbar. Top: search bar (case-insensitive substring over
// relative paths — results flatten, Esc restores the folder context) + refresh.
// Footer: selection summary with batch Reimport / Copy UUIDs.
//
// Wired to the AssetDatabase (AssetManager::Instance) — entries carry UUID, type,
// dirty flag and size; .meta sidecars are managed by the DB rescan. File-level ops
// (create/rename/duplicate/delete) go through editor/core/AssetOperations and are
// deliberately NOT undoable scene commands (§10.1: hard delete, documented).
//
// Drag sources emit the shared ASSET_UUID payload (editor/core/AssetDragDrop.h);
// hierarchy rows consume it to instantiate models (§5.4). Keyboard: F2 rename
// (primary selection), Del deletes (confirmed), Ctrl+D duplicates — the panel
// reports window focus through Config::focusSinkFn so the app can yield these keys.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"

#include "editor/panels/Panel.h"

namespace raven::editor {

class ProjectPanel final : public Panel {
public:
    struct Config {
        bool* openFlag = nullptr;
        std::function<std::filesystem::path()> assetsDirFn;    // DB root (project "assets/")
        std::function<float()> rescanSecondsFn;                // auto-rescan cadence; default 2s
        std::function<void(const std::filesystem::path&)> openSceneFn;   // double-click a .ravenscene
        std::function<void(bool)> focusSinkFn;                 // panel reports keyboard focus each frame
        std::function<void()> afterFileOpFn;                   // optional hook after mutating ops
    };

    explicit ProjectPanel(Config config);

    const char* Name() const override { return "Assets"; }
    bool IsOpen() const override { return m_Config.openFlag ? *m_Config.openFlag : true; }
    void SetOpen(bool open) override { if (m_Config.openFlag) *m_Config.openFlag = open; }

    void Draw() override;

private:
    struct Entry {
        std::string relPath;          // forward slashes, assets-root relative ("textures/crate.png")
        std::string name;             // file or folder name only
        std::filesystem::path absolute;
        bool isDirectory = false;
        raven::AssetType type = raven::AssetType::Unknown;
        raven::UUID uuid;             // nil when not in the DB (unknown types are skipped)
        bool isDirty = false;
        uint64_t sizeBytes = 0;
    };

    void Rebuild();                          // rescan DB + rebuild entry list (m_Dirty or timer)
    void RescanAndRefresh();
    void DrawToolbar();
    void DrawCrumbBar();
    void DrawFolderTree();                   // left pane
    void DrawEntryTreeNode(const std::string& relFolder);
    void DrawEntriesPane();                  // right pane grid/list
    void DrawEntryGrid(const std::vector<const Entry*>& shown);
    void DrawEntryList(const std::vector<const Entry*>& shown);
    void DrawEntryFooter();
    void DrawContextMenus(const Entry* item);      // background + item popups
    void DrawDeleteModal();
    void DrawRenameEditor(const Entry& entry, float width);

    bool IsSelected(const std::string& relPath) const { return m_Selected.find(relPath) != m_Selected.end(); }
    void ClickSelect(const Entry& entry);                 // plain / ctrl-click semantics
    void SyncAssetSelection();                          // mirror primary selection to resource-mode asset selection
    std::vector<const Entry*> VisibleEntries() const;     // current folder children, or search results
    std::vector<std::string> ChildFolders(const std::string& relFolder) const;
    const Entry* FindByRel(const std::string& relPath) const;
    void BeginInlineRename(const Entry& entry);

    Config m_Config;

    std::vector<Entry> m_Entries;            // full snapshot (dirs + known assets)
    std::unordered_set<std::string> m_Folders;          // all folder rel paths (for the tree)
    std::unordered_set<std::string> m_Expanded;         // expanded folders (seed for the tree)
    std::unordered_set<std::string> m_Selected;         // selected rel paths
    std::string m_CurrentFolder;             // "" = root
    std::string m_Primary;                   // most-recently selected (drag source on multi-select)
    int m_TreeEpoch = 0;                     // bumped by Collapse/Expand-all; resets node ImGui state

    char m_FilterBuf[128] = "";
    bool m_ListView = false;
    bool m_Dirty = true;                     // rebuild requested
    double m_LastScan = -1000.0;

    std::string m_Renaming;                  // relPath being renamed ("" = none)
    char m_RenameBuf[256] = "";
    std::vector<std::string> m_DeleteQueue;  // absolute paths pending the confirm modal
};

} // namespace raven::editor
