#include "editor/widgets/ThumbnailCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include "raven/core/EventBus.h"
#include "raven/core/JobSystem.h"
#include "raven/core/Logger.h"

#include "editor/events/EditorSignals.h"
#include "editor/widgets/ThumbnailGenerator.h"

namespace raven::editor {

namespace {
// §10.2: cap in-flight bakes at 2× the worker count.
uint32_t BakeCap() {
    const uint32_t workers = raven::JobSystem::Instance().IsInitialized()
                                 ? raven::JobSystem::Instance().WorkerCount() : 1;
    return 2 * std::max(1u, workers);
}
} // namespace

ThumbnailCache& ThumbnailCache::Instance() {
    static ThumbnailCache instance;
    return instance;
}

std::string ThumbnailCache::UuidKey(const raven::UUID& uuid) { return uuid.ToString(); }

std::filesystem::path ThumbnailCache::DiskPath(const raven::UUID& uuid) const {
    return m_ThumbnailDir / (UuidKey(uuid) + ".png");
}

uint64_t ThumbnailCache::FileMtimeNs(const std::filesystem::path& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    return static_cast<uint64_t>(std::max<int64_t>(0, ns));
}

void ThumbnailCache::Initialize(const std::filesystem::path& projectDir, raven::Renderer* renderer) {
    Shutdown();   // safe re-init (project switch)
    if (!renderer) return;

    m_ThumbnailDir = projectDir / ".raven" / "thumbnails";
    std::error_code ec;
    std::filesystem::create_directories(m_ThumbnailDir, ec);
    if (ec) RAVEN_LOG_WARN("Editor: thumbnail cache dir unavailable (%s): %s", m_ThumbnailDir.string().c_str(), ec.message().c_str());

    m_Renderer = renderer;
    // Asset hot-reload → drop the affected thumbnail (and any material thumbnails that
    // depend on it) so the grid re-bakes it. Signals arrive on the main thread at Drain.
    m_SignalSub = raven::EventBus::Instance().Subscribe<AssetChangedSignal>(
        [this](const AssetChangedSignal& signal) {
            if (signal.kind == raven::AssetReloadKind::Removed ||
                signal.kind == raven::AssetReloadKind::Modified ||
                signal.kind == raven::AssetReloadKind::Added) {
                Invalidate(signal.asset);
                InvalidateDependents(signal.asset);
            }
        });
    m_Initialized = true;
    RAVEN_LOG_INFO("Editor: thumbnail cache at %s", m_ThumbnailDir.string().c_str());
}

void ThumbnailCache::Shutdown() {
    if (m_SignalSub != 0) {
        raven::EventBus::Instance().Unsubscribe(m_SignalSub);
        m_SignalSub = 0;
    }
    // Discard completions that landed after the last frame; jobs never touch the GPU or
    // the entry map, so dropping them is safe once the device is about to go away.
    {
        std::lock_guard<std::mutex> lock(m_CompletionsMutex);
        m_Completions.clear();
    }
    for (auto& [uuid, entry] : m_Entries) ReleaseGpu(entry);
    m_Entries.clear();
    m_Renderer = nullptr;
    m_Initialized = false;
}

void ThumbnailCache::ReleaseGpu(Entry& entry) {
    if (entry.descriptor != VK_NULL_HANDLE) { ImGui_ImplVulkan_RemoveTexture(entry.descriptor); entry.descriptor = VK_NULL_HANDLE; }
    if (entry.image.image != VK_NULL_HANDLE && m_Renderer) m_Renderer->DestroyUiImage(entry.image);
    else entry.image = {};
    entry.state = ThumbnailState::Idle;
    entry.width = entry.height = 0;
}

void ThumbnailCache::Invalidate(const raven::UUID& uuid) {
    auto it = m_Entries.find(uuid);
    if (it != m_Entries.end()) ReleaseGpu(it->second);
    // Tell pending bakes for this uuid to yield? They complete into the map blindly;
    // DrainCompletions discards completions whose entry is no longer Pending.
    std::error_code ec;
    std::filesystem::remove(DiskPath(uuid), ec);
}

void ThumbnailCache::InvalidateDependents(const raven::UUID& uuid) {
    for (auto& [id, entry] : m_Entries) {
        if (id == uuid) continue;
        for (const raven::UUID& dep : entry.deps) {
            if (dep == uuid) { ReleaseGpu(entry); std::error_code ec; std::filesystem::remove(DiskPath(id), ec); break; }
        }
    }
}

void ThumbnailCache::KickTexture(const raven::UUID& uuid, const std::filesystem::path& source, uint64_t sourceMtime) {
    auto job = [this, uuid, source, sourceMtime]() {
        Completion completion;
        completion.uuid = uuid;
        completion.sourceMtime = sourceMtime;

        const thumbnail::BakeResult baked = thumbnail::BakeTexture(source);
        if (!baked.ok) {
            completion.ok = false;
            completion.error = baked.error;
        } else {
            // Persist before reporting so a crash still leaves the cache warm.
            std::error_code ec;
            std::filesystem::create_directories(m_ThumbnailDir, ec);
            const std::filesystem::path out = DiskPath(uuid);
            std::string writeError;
            if (!thumbnail::WritePngAtomic(out, baked.width, baked.height, baked.rgba.data(), &writeError))
                RAVEN_LOG_WARN("Editor: thumbnail bake of %s produced no cache entry (%s)", out.string().c_str(), writeError.c_str());
            completion.ok = true;
            completion.width = baked.width;
            completion.height = baked.height;
            completion.pixels = std::move(baked.rgba);   // moved under the completions lock below
            completion.deps = baked.deps;
        }
        std::lock_guard<std::mutex> lock(m_CompletionsMutex);
        m_Completions.push_back(std::move(completion));
    };

    if (raven::JobSystem::Instance().IsInitialized()) raven::JobSystem::Instance().Submit(job);
    else job();   // headless/test bring-up without workers: bake inline
}

void ThumbnailCache::KickOrSync(const raven::UUID& uuid, const std::filesystem::path& source,
                                raven::AssetType type, uint64_t sourceMtime) {
    Entry& entry = m_Entries[uuid];
    if (type == raven::AssetType::Texture) KickTexture(uuid, source, sourceMtime);
    else {
        // §10.2 model/material preview-scene bakes land with the offscreen preview
        // scene; until then report Failed so the grid shows the type glyph.
        entry.state = ThumbnailState::Failed;
        entry.sourceMtime = sourceMtime;
        entry.lastWrite = sourceMtime;
        RAVEN_LOG_INFO("Editor: thumbnails for %s assets are not implemented yet — using type glyph",
                       raven::AssetTypeToString(type));
        return;
    }
    entry.state = ThumbnailState::Pending;
    entry.sourceMtime = sourceMtime;
}

bool ThumbnailCache::HydrateFromDisk(const raven::UUID& uuid, Entry& entry, uint64_t sourceMtime, uint64_t diskMtime) {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels = thumbnail::LoadPng(DiskPath(uuid), &width, &height);
    if (pixels.empty()) return false;
    if (!m_Renderer) return false;

    raven::Renderer::UiImage image;
    std::string uploadError;
    if (!m_Renderer->CreateUiImage(pixels.data(), width, height, &image, &uploadError)) {
        RAVEN_LOG_WARN("Editor: thumbnail upload failed for %s: %s", uuid.ToString().c_str(), uploadError.c_str());
        return false;
    }
    entry.image = image;
    entry.descriptor = ImGui_ImplVulkan_AddTexture(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    entry.width = width;
    entry.height = height;
    entry.state = ThumbnailState::Ready;
    entry.sourceMtime = sourceMtime;
    entry.lastWrite = diskMtime;
    return true;
}

ThumbnailState ThumbnailCache::Get(const raven::UUID& uuid, const std::filesystem::path& source,
                                   raven::AssetType type, ImTextureID* outTexture, float* outAspect) {
    if (!m_Initialized || uuid.IsNil()) return ThumbnailState::Failed;

    Entry& entry = m_Entries[uuid];
    const uint64_t sourceMtime = FileMtimeNs(source);

    if (entry.state == ThumbnailState::Ready || entry.state == ThumbnailState::Failed) {
        if (entry.sourceMtime != sourceMtime) {
            // Source changed on disk — rebake (or drop, for removed files).
            ReleaseGpu(entry);
            std::error_code ec;
            std::filesystem::remove(DiskPath(uuid), ec);
        } else {
            if (outTexture && entry.state == ThumbnailState::Ready) *outTexture = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(entry.descriptor));
            if (outAspect && entry.height > 0) *outAspect = static_cast<float>(entry.width) / static_cast<float>(entry.height);
            return entry.state;
        }
    }
    if (entry.state == ThumbnailState::Pending) return ThumbnailState::Pending;

    // Disk cache: fresh PNG from a previous session hydrates without a bake.
    const std::filesystem::path disk = DiskPath(uuid);
    const uint64_t diskMtime = FileMtimeNs(disk);
    if (diskMtime != 0 && diskMtime >= sourceMtime && HydrateFromDisk(uuid, entry, sourceMtime, diskMtime)) {
        if (outTexture) *outTexture = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(entry.descriptor));
        if (outAspect && entry.height > 0) *outAspect = static_cast<float>(entry.width) / static_cast<float>(entry.height);
        return ThumbnailState::Ready;
    }

    // Kick a bake, honoring the in-flight cap. The entry flips Pending immediately;
    // the job posts its result to the completion queue.
    size_t pending = 0;
    for (const auto& [id, e] : m_Entries) pending += (e.state == ThumbnailState::Pending ? 1 : 0);
    if (pending >= BakeCap()) return ThumbnailState::Pending;   // retry next frame

    KickOrSync(uuid, source, type, sourceMtime);
    return entry.state == ThumbnailState::Failed ? ThumbnailState::Failed : ThumbnailState::Pending;
}

void ThumbnailCache::DrainCompletions() {
    std::vector<Completion> completions;
    {
        std::lock_guard<std::mutex> lock(m_CompletionsMutex);
        completions.swap(m_Completions);
    }
    for (Completion& completion : completions) {
        auto it = m_Entries.find(completion.uuid);
        if (it == m_Entries.end() || it->second.state != ThumbnailState::Pending)
            continue;   // invalidated while baking — discard
        Entry& entry = it->second;
        if (!completion.ok) {
            entry.state = ThumbnailState::Failed;
            entry.sourceMtime = completion.sourceMtime;
            entry.lastWrite = FileMtimeNs(DiskPath(completion.uuid));
            RAVEN_LOG_INFO("Editor: thumbnail bake failed for %s: %s", completion.uuid.ToString().c_str(), completion.error.c_str());
            continue;
        }
        if (!m_Renderer) { entry.state = ThumbnailState::Failed; continue; }

        raven::Renderer::UiImage image;
        std::string uploadError;
        if (!m_Renderer->CreateUiImage(completion.pixels.data(), completion.width, completion.height, &image, &uploadError)) {
            entry.state = ThumbnailState::Failed;
            RAVEN_LOG_WARN("Editor: thumbnail upload failed for %s: %s", completion.uuid.ToString().c_str(), uploadError.c_str());
            continue;
        }
        entry.image = image;
        entry.descriptor = ImGui_ImplVulkan_AddTexture(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        entry.width = completion.width;
        entry.height = completion.height;
        entry.state = ThumbnailState::Ready;
        entry.sourceMtime = completion.sourceMtime;
        entry.lastWrite = FileMtimeNs(DiskPath(completion.uuid));
        entry.deps = std::move(completion.deps);
    }
}

} // namespace raven::editor
