// RavenEditor — Texture Remap dialog (EDITOR_DESIGN §10.4).
//
// Modal, non-docked. Rows per material slot (Diffuse … Height): detected candidates
// ranked by name rules (engine/assets/TextureNameRules), a combo to pick one (or an
// explicit skip), a Browse… fallback listing every texture in the project database,
// and a status icon per row. Import stays disabled until every row carries a decision
// (pick or explicit skip). Commit writes the `.meta` `textureRemap` JSON section of
// the model and enqueues a reimport through the import-progress tracker.
//
// Results persist across sessions via the model's .meta — reopening the dialog
// pre-fills the persisted decisions and never re-asks for a decided slot.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "raven/assets/AssetTypes.h"
#include "raven/assets/TextureNameRules.h"
#include "raven/core/UUID.h"

namespace raven::editor {

class TextureRemapDialog {
public:
    static TextureRemapDialog& Instance();

    // modelAbs: absolute path of the model file; uuid: its DB identity (ImportProgress
    // needs it for the reimport). Safe to call when the asset manager is down — the
    // dialog opens with only the persisted decisions and skip options.
    void Open(const std::filesystem::path& modelAbs, const raven::UUID& modelUuid,
              const std::string& modelDisplay);

    void Draw();   // no-op unless open; call once per frame from the app
    bool IsOpen() const { return m_Open; }

private:
    struct Candidate {
        std::string path;   // assets-root-relative (what .meta textureRemap stores)
        int score = 0;
    };
    struct Row {
        raven::TextureSlot slot = raven::TextureSlot::Diffuse;
        std::vector<Candidate> candidates;   // ranked, best first; an explicit pick prepends
        int choice = -1;                     // -1 undecided, -2 explicit skip, >= 0 candidate index
    };

    void RefreshTextureList();
    void PrefillFromMeta();
    bool AllDecided() const;
    void Commit();
    void BrowseFor(int row);

    static constexpr int kUndecided = -1;
    static constexpr int kSkip = -2;

    bool m_Open = false;
    std::filesystem::path m_ModelAbs;
    raven::UUID m_ModelUuid;
    std::string m_ModelDisplay;
    std::vector<Row> m_Rows;
    std::vector<std::string> m_TexturePaths; // every texture in the DB (browse list)
    int m_BrowseRow = -1;                    // row whose browse list is expanded; -1 none
    char m_BrowseFilter[128] = {};
    std::string m_Error;
};

} // namespace raven::editor
