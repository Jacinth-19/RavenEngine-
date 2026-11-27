// RavenEditor — asset import progress toast (EDITOR_DESIGN §10.3).
//
// Owns the set of in-flight async imports (AssetManager::ReloadAsync / LoadAsync)
// and draws a compact undocked toast with a determinate bar while they run.
// Completion is applied on the main thread once per frame (Drain): each finished
// job publishes AssetChangedSignal{Modified} so thumbnails/material caches
// invalidate. The toast lingers briefly after the last job
// (longer on failure, so the error is readable) and disappears otherwise.
//
// Polling the futures directly is deliberate — the bus is edge-triggered and this
// needs per-frame progress state; ImportReportSignal is published on bulk
// completion for the components that care about outcomes, not progress.
#pragma once

#include <cstdint>
#include <deque>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

#include "raven/assets/AssetManager.h"
#include "raven/core/UUID.h"

namespace raven::editor {

class ImportProgress {
public:
    static ImportProgress& Instance();

    // Enqueues a force-reimport of `uuid` on a worker. `label` is shown in the toast.
    // Concurrent reloads of the same asset never overlap: a request that arrives
    // while one is in flight folds into a single follow-up run (last write wins).
    void EnqueueReload(const std::string& label, const raven::UUID& uuid);
    // Enqueues a plain async load (project bring-up prewarming).
    void EnqueueLoad(const std::string& label, const raven::UUID& uuid);

    void Drain();    // main thread, once per frame: retire finished jobs, publish signals
    void Draw();     // floating toast; call once per frame when the project is loaded
    void Shutdown(); // called from EditorApp::Shutdown — drops pending jobs (their results are discarded)

    bool Busy() const { return !m_Jobs.empty(); }
    int TotalJobs() const { return m_Total; }
    int DoneJobs() const { return m_Done; }
    int FailedJobs() const { return m_Failed; }

private:
    struct Job {
        std::string label;
        raven::UUID uuid;
        bool reload = false;
        std::future<raven::AssetManager::LoadResult> future;
    };

    ImportProgress() = default;

    static float HoldSeconds(bool failed);   // toast linger time after completion

    std::deque<Job> m_Jobs;
    // Per-UUID folding: at most one reload job per asset in flight; requests that
    // arrive while it runs fold into a single follow-up run (asset artifacts are
    // written by truncation, so concurrent reloads of one asset would race).
    std::unordered_set<raven::UUID, raven::UUIDHash> m_Requeue;
    int m_Total = 0;
    int m_Done = 0;
    int m_Failed = 0;
    float m_HoldTimer = 0.0f;                // seconds of post-completion visibility remaining
    bool m_Showing = false;                  // toast currently on screen (hysteresis across frames)
    std::string m_LastError;
    bool m_HoldFailed = false;               // set at bulk completion; drives the lingering toast
    std::string m_HoldError;
};

} // namespace raven::editor
