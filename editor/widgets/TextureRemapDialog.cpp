#include "editor/widgets/TextureRemapDialog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "raven/assets/AssetDatabase.h"
#include "raven/assets/AssetManager.h"
#include "raven/assets/AssetMeta.h"
#include "raven/core/Logger.h"

#include "editor/Icons.h"
#include "editor/core/TextureRemap.h"
#include "editor/widgets/ImportProgress.h"

namespace raven::editor {

namespace {
const raven::TextureSlot kRowSlots[] = {
    raven::TextureSlot::Diffuse, raven::TextureSlot::Normal, raven::TextureSlot::Roughness,
    raven::TextureSlot::Metalness, raven::TextureSlot::Emissive, raven::TextureSlot::Occlusion,
    raven::TextureSlot::Height,
};

std::string StemOf(const std::string& path) {
    std::string stem = path;
    const size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return stem;
}
} // namespace

TextureRemapDialog& TextureRemapDialog::Instance() {
    static TextureRemapDialog instance;
    return instance;
}

void TextureRemapDialog::Open(const std::filesystem::path& modelAbs, const raven::UUID& modelUuid,
                              const std::string& modelDisplay) {
    m_ModelAbs = modelAbs;
    m_ModelUuid = modelUuid;
    m_ModelDisplay = modelDisplay;
    m_Error.clear();
    m_BrowseRow = -1;
    m_BrowseFilter[0] = '\0';

    RefreshTextureList();

    // Build one row per slot; candidates ranked by name rules against the DB textures.
    m_Rows.clear();
    for (const raven::TextureSlot slot : kRowSlots) {
        Row row;
        row.slot = slot;
        for (const std::string& path : m_TexturePaths) {
            const int score = raven::ScoreTextureForSlot(StemOf(path), slot);
            if (score > 0) row.candidates.push_back({path, score});
        }
        std::stable_sort(row.candidates.begin(), row.candidates.end(),
                         [](const Candidate& a, const Candidate& b) {
                             if (a.score != b.score) return a.score > b.score;
                             return a.path < b.path;
                         });
        if (row.candidates.size() > 8) row.candidates.resize(8);
        m_Rows.push_back(std::move(row));
    }

    PrefillFromMeta();
    m_Open = true;
    ImGui::OpenPopup("TextureRemap");
}

void TextureRemapDialog::RefreshTextureList() {
    m_TexturePaths.clear();
    raven::AssetManager& manager = raven::AssetManager::Instance();
    if (!manager.IsInitialized()) return;
    for (const auto* entry : manager.Database().EntriesOfType(raven::AssetType::Texture))
        if (entry) m_TexturePaths.push_back(entry->relativePath);
}

void TextureRemapDialog::PrefillFromMeta() {
    const std::filesystem::path metaPath = raven::MetaPathFor(m_ModelAbs);
    raven::AssetMeta meta;
    std::string err;
    if (auto loaded = raven::ReadMeta(metaPath, &err)) meta = std::move(*loaded);
    else return;   // no sidecar yet — everything starts undecided

    for (const auto& [slot, chosen] : raven::editor::DecisionsBySlot(meta.textureRemap)) {
        for (Row& row : m_Rows) {
            if (row.slot != slot) continue;
            if (chosen.empty()) { row.choice = kSkip; continue; }
            auto it = std::find_if(row.candidates.begin(), row.candidates.end(),
                                   [&](const Candidate& c) { return c.path == chosen; });
            if (it != row.candidates.end()) {
                row.choice = static_cast<int>(it - row.candidates.begin());
            } else {
                row.candidates.insert(row.candidates.begin(), {chosen, /*explicit*/ 1000});
                row.choice = 0;
            }
        }
    }
}

bool TextureRemapDialog::AllDecided() const {
    for (const Row& row : m_Rows) if (row.choice == kUndecided) return false;
    return true;
}

void TextureRemapDialog::Commit() {
    std::vector<RemapDecision> decisions;
    decisions.reserve(m_Rows.size());
    for (const Row& row : m_Rows) {
        if (row.choice == kSkip) decisions.push_back({row.slot, ""});
        else if (row.choice >= 0 && row.choice < static_cast<int>(row.candidates.size()))
            decisions.push_back({row.slot, row.candidates[row.choice].path});
    }

    const std::filesystem::path metaPath = raven::MetaPathFor(m_ModelAbs);
    std::string err;
    raven::AssetMeta meta;
    if (auto loaded = raven::ReadMeta(metaPath, &err)) {
        meta = std::move(*loaded);
    } else {
        // Sidecar missing (model registered but never scanned/imported) — create a minimal one.
        std::error_code ec;
        meta.uuid = m_ModelUuid;
        meta.type = raven::AssetType::Mesh;
        raven::AssetManager& manager = raven::AssetManager::Instance();
        if (manager.IsInitialized())
            meta.relativePath = std::filesystem::relative(m_ModelAbs, manager.Database().AssetsRoot(), ec).generic_string();
        else
            meta.relativePath = m_ModelAbs.filename().generic_string();
        meta.importSettings = raven::DefaultImportSettings(raven::AssetType::Mesh);
    }
    meta.textureRemap = RemapToJson(decisions);

    if (!raven::WriteMeta(metaPath, meta, &err)) {
        m_Error = "cannot write " + metaPath.string() + ": " + err;
        return;
    }
    RAVEN_LOG_INFO("Assets: texture remap saved for '%s' (%zu decision(s)); reimport queued",
                   m_ModelDisplay.c_str(), decisions.size());
    ImportProgress::Instance().EnqueueReload(m_ModelDisplay, m_ModelUuid);
    m_Open = false;
    ImGui::CloseCurrentPopup();   // commits happen inside the modal — dismiss it here
}

void TextureRemapDialog::Draw() {
    if (!m_Open) return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(760.0f, 640.0f));
    if (!ImGui::BeginPopupModal("TextureRemap", nullptr, ImGuiWindowFlags_NoDocking)) return;

    ImGui::Text(ICON_ASSET_MESH "  Texture Remap — %s", m_ModelDisplay.c_str());
    ImGui::TextDisabled("Map material slots to project textures. Scores come from file-name rules.");
    ImGui::TextDisabled("Decisions persist in %s.meta and are never re-asked.", m_ModelDisplay.c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("textureRemapTable", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Detected candidates", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        for (int i = 0; i < static_cast<int>(m_Rows.size()); ++i) {
            Row& row = m_Rows[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(raven::SlotToString(row.slot));

            ImGui::TableSetColumnIndex(1);
            // Combo: candidates (with score hints) + explicit skip + undecided sentinel.
            const char* preview = "undecided…";
            if (row.choice == kSkip) preview = "(no texture)";
            else if (row.choice >= 0 && row.choice < static_cast<int>(row.candidates.size())) preview = row.candidates[row.choice].path.c_str();
            if (ImGui::BeginCombo("##candidates", preview)) {
                if (ImGui::Selectable("(no texture)", row.choice == kSkip)) row.choice = kSkip;
                for (int c = 0; c < static_cast<int>(row.candidates.size()); ++c) {
                    char label[512];
                    std::snprintf(label, sizeof(label), "%s   (%d)", row.candidates[c].path.c_str(), row.candidates[c].score);
                    if (ImGui::Selectable(label, row.choice == c)) row.choice = c;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Browse…")) {
                m_BrowseRow = (m_BrowseRow == i) ? -1 : i;
                m_BrowseFilter[0] = '\0';
            }

            ImGui::TableSetColumnIndex(2);
            if (row.choice == kUndecided) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), "%s ?", ICON_WARNING);
            else if (row.choice == kSkip) ImGui::TextDisabled("—");
            else ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), ICON_CHECK);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Browse section: full DB texture list, filtered. Selecting applies to the row.
    if (m_BrowseRow >= 0 && m_BrowseRow < static_cast<int>(m_Rows.size())) {
        ImGui::Separator();
        ImGui::Text("Browse textures — choosing for %s", raven::SlotToString(m_Rows[m_BrowseRow].slot));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##browseFilter", "filter…", m_BrowseFilter, sizeof(m_BrowseFilter));
        std::string needle = m_BrowseFilter;
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto lower = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };
        if (ImGui::BeginChild("##browseList", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders)) {
            int shown = 0;
            for (const std::string& path : m_TexturePaths) {
                if (!needle.empty() && lower(path).find(needle) == std::string::npos) continue;
                if (++shown > 128) { ImGui::TextDisabled("…"); break; }
                if (ImGui::Selectable(path.c_str())) {
                    Row& row = m_Rows[m_BrowseRow];
                    auto it = std::find_if(row.candidates.begin(), row.candidates.end(),
                                           [&](const Candidate& c) { return c.path == path; });
                    if (it != row.candidates.end()) row.choice = static_cast<int>(it - row.candidates.begin());
                    else { row.candidates.insert(row.candidates.begin(), {path, 1000}); row.choice = 0; }
                    m_BrowseRow = -1;
                    break;
                }
            }
            if (shown == 0) ImGui::TextDisabled("(no textures match%s)", needle.empty() ? " — is the database empty?" : "");
        }
        ImGui::EndChild();
    }

    if (!m_Error.empty()) { ImGui::Separator(); ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", m_Error.c_str()); }

    ImGui::Separator();
    const bool canCommit = AllDecided();
    ImGui::BeginDisabled(!canCommit);
    if (ImGui::Button("Import", ImVec2(120.0f, 0.0f))) Commit();
    ImGui::EndDisabled();
    if (!canCommit && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Choose a candidate or '(no texture)' for every slot first");
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace raven::editor
