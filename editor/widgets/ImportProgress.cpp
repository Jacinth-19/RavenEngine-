#include "editor/widgets/ImportProgress.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "imgui.h"

#include "raven/core/EventBus.h"
#include "raven/core/Logger.h"

#include "editor/Icons.h"
#include "editor/events/EditorSignals.h"

namespace raven::editor {

ImportProgress& ImportProgress::Instance() {
    static ImportProgress instance;
    return instance;
}

void ImportProgress::EnqueueReload(const std::string& label, const raven::UUID& uuid) {
    // A reload for this asset is already running: fold this request into one
    // follow-up run (started when the in-flight job retires in Drain) instead of
    // racing two truncations of the same artifact file.
    for (const Job& existing : m_Jobs) {
        if (existing.reload && existing.uuid == uuid) {
            m_Requeue.insert(uuid);
            return;
        }
    }
    Job job;
    job.label = label;
    job.uuid = uuid;
    job.reload = true;
    job.future = raven::AssetManager::Instance().ReloadAsync(uuid);
    m_Jobs.push_back(std::move(job));
    ++m_Total;
    m_Showing = true;   // surface the toast immediately on enqueue
}

void ImportProgress::EnqueueLoad(const std::string& label, const raven::UUID& uuid) {
    Job job;
    job.label = label;
    job.uuid = uuid;
    job.reload = false;
    job.future = raven::AssetManager::Instance().LoadAsync(uuid);
    m_Jobs.push_back(std::move(job));
    ++m_Total;
    m_Showing = true;
}

float ImportProgress::HoldSeconds(bool failed) { return failed ? 6.0f : 1.75f; }

void ImportProgress::Drain() {
    const float deltaSeconds = ImGui::GetIO().DeltaTime;

    if (m_Jobs.empty()) {
        if (m_HoldTimer > 0.0f) {
            m_HoldTimer -= deltaSeconds;
            if (m_HoldTimer <= 0.0f) m_Showing = false;
        }
        return;
    }

    for (auto it = m_Jobs.begin(); it != m_Jobs.end();) {
        std::future_status status = it->future.wait_for(std::chrono::seconds(0));
        if (status != std::future_status::ready) { ++it; continue; }

        raven::AssetManager::LoadResult result = it->future.get();
        const std::string label = it->label;
        const raven::UUID uuid = it->uuid;
        const bool wasReload = it->reload;
        ++m_Done;
        if (!result.ok) {
            ++m_Failed;
            m_LastError = result.error;
            RAVEN_LOG_ERROR("Assets: reimport of '%s' failed: %s", it->label.c_str(), result.error.c_str());
        } else {
            RAVEN_LOG_INFO("Assets: reimported '%s'", it->label.c_str());
        }
        // Transactional outcome for this asset — thumbnails/material caches re-bake.
        AssetChangedSignal signal;
        signal.asset = uuid;
        signal.kind = raven::AssetReloadKind::Modified;
        raven::EventBus::Instance().Publish(signal);
        it = m_Jobs.erase(it);

        // A folded reload was requested while this job ran — run it now so the
        // latest on-disk state gets imported exactly once.
        if (wasReload && m_Requeue.erase(uuid) > 0) EnqueueReload(label, uuid);
    }

    if (m_Jobs.empty()) {
        // Bulk completion notice for anything subscribed to outcomes.
        ImportReportSignal report;
        m_HoldFailed = m_Failed > 0;
        if (m_HoldFailed) report.summary = "Import finished: " + std::to_string(m_Failed) + " of " + std::to_string(m_Total) + " failed (" + m_LastError + ")";
        else report.summary = "Import finished: " + std::to_string(m_Done) + " asset(s)";
        raven::EventBus::Instance().Publish(report);
        if (m_HoldFailed) RAVEN_LOG_ERROR("Assets: %s", report.summary.c_str());
        m_HoldError = m_LastError;
        m_HoldTimer = HoldSeconds(m_HoldFailed);
        m_Done = 0;
        m_Failed = 0;
        m_Total = 0;
        m_LastError.clear();
    }
}

void ImportProgress::Draw() {
    if (!m_Showing) return;

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float toastWidth = 336.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - toastWidth - 16.0f, 34.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##import_progress_toast", nullptr, flags)) { ImGui::End(); return; }

    if (!m_Jobs.empty()) {
        const int finished = m_Done + m_Failed;
        const float fraction = m_Total > 0 ? static_cast<float>(finished) / static_cast<float>(m_Total) : 0.0f;
        ImGui::TextUnformatted(ICON_REFRESH "  Importing assets…");
        ImGui::ProgressBar(fraction, ImVec2(toastWidth - 16.0f, 0.0f),
                           (std::to_string(finished) + "/" + std::to_string(m_Total)).c_str());
        // Name the first still-running job — the queue is small in practice.
        ImGui::TextDisabled("%s", m_Jobs.front().label.c_str());
    } else if (!m_HoldFailed) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.55f, 1.0f), "%s  Import complete", ICON_REFRESH);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s  Import failed", ICON_REFRESH);
        if (!m_HoldError.empty()) ImGui::TextWrapped("%s", m_HoldError.c_str());
    }
    ImGui::End();
}

void ImportProgress::Shutdown() {
    m_Jobs.clear();   // futures discarded — late results are dropped
    m_Total = m_Done = m_Failed = 0;
    m_HoldTimer = 0.0f;
    m_Showing = false;
    m_HoldFailed = false;
    m_HoldError.clear();
}

} // namespace raven::editor
