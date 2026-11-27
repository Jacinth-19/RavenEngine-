// RavenEditor — thumbnail cache for the asset browser (EDITOR_DESIGN §10.2).
//
// In-memory map UUID -> entry { pixels/GPU texture, state, lastWrite }; states
// Idle/Pending/Ready/Failed. Bakes run on JobSystem workers (never the UI thread);
// finished bakes are applied by DrainCompletions() on the UI thread once per frame.
// A persisted PNG cache lives at <project>/.raven/thumbnails/<uuid>.png and is
// re-hydrated lazily per Get() (tiny decode inline) — textures bake through
// stb_image + aspect-preserving fit into a 128px box and are written atomically.
//
// Cascade invalidation: material bakes record the textures they reference in
// Entry::deps; an AssetChangedSignal{uuid} invalidates every entry whose dep set
// contains uuid (plus the entry itself). Scene-saved events never invalidate.
//
// Model/material preview-scene bakes are not implemented yet; those entries land in
// the Failed state (UI falls back to the type glyph) rather than blocking the grid.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"
#include "raven/renderer/Renderer.h"

namespace raven { class Renderer; }

namespace raven::editor {

enum class ThumbnailState : uint8_t {
    Idle,
    Pending,    // bake enqueued or in flight
    Ready,      // ImTextureID valid; pixels live on the GPU
    Failed,     // bake failed (unsupported format, decode error, …) — UI shows type glyph
};

class ThumbnailCache {
public:
    static ThumbnailCache& Instance();

    // <projectDir>/.raven/thumbnails is the disk cache location. Re-initializes cleanly
    // (any prior GPU resources are released first). May be called again on project switch.
    void Initialize(const std::filesystem::path& projectDir, raven::Renderer* renderer);
    void Shutdown();
    bool IsInitialized() const { return m_Initialized; }

    // UI thread, non-blocking. Ensures a thumbnail for (uuid, source): enqueues a bake
    // (or hydrates the disk cache) and reports the current state. Out parameters are
    // filled only when Ready. aspect = width / (float)height of the decoded image.
    ThumbnailState Get(const raven::UUID& uuid, const std::filesystem::path& source,
                       raven::AssetType type, ImTextureID* outTexture, float* outAspect);

    // Drop the entry for uuid (deletes the disk PNG, releasing its GPU resources).
    void Invalidate(const raven::UUID& uuid);
    // Drop every entry that lists uuid in its dependency set.
    void InvalidateDependents(const raven::UUID& uuid);

    // Apply finished bakes: uploads pixels to the GPU and flips entries to Ready.
    // Main thread, call once per frame.
    void DrainCompletions();

private:
    struct Entry {
        ThumbnailState state = ThumbnailState::Idle;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t sourceMtime = 0;           // ns since epoch at bake/hydrate time
        uint64_t lastWrite = 0;             // ns; bumped on every transition to Ready/Failed
        VkDescriptorSet descriptor = VK_NULL_HANDLE;   // ImGui_ImplVulkan_AddTexture token
        raven::Renderer::UiImage image{};
        std::vector<raven::UUID> deps;      // texture deps (materials); empty for textures
    };

    struct Completion {
        raven::UUID uuid;
        bool ok = false;
        std::string error;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;        // RGBA8, tightly packed
        uint64_t sourceMtime = 0;
        std::vector<raven::UUID> deps;
    };

    ThumbnailCache() = default;
    ~ThumbnailCache() { Shutdown(); }

    static std::string UuidKey(const raven::UUID& uuid);
    std::filesystem::path DiskPath(const raven::UUID& uuid) const;
    static uint64_t FileMtimeNs(const std::filesystem::path& path);

    void KickTexture(const raven::UUID& uuid, const std::filesystem::path& source, uint64_t sourceMtime);
    void KickOrSync(const raven::UUID& uuid, const std::filesystem::path& source,
                    raven::AssetType type, uint64_t sourceMtime);
    bool HydrateFromDisk(const raven::UUID& uuid, Entry& entry, uint64_t sourceMtime, uint64_t diskMtime);
    void ReleaseGpu(Entry& entry);

    std::unordered_map<raven::UUID, Entry, raven::UUIDHash> m_Entries;
    std::mutex m_CompletionsMutex;
    std::vector<Completion> m_Completions;

    std::filesystem::path m_ThumbnailDir;
    raven::Renderer* m_Renderer = nullptr;
    uint64_t m_SignalSub = 0;      // EventBus subscription id for AssetChangedSignal
    bool m_Initialized = false;
};

} // namespace raven::editor
