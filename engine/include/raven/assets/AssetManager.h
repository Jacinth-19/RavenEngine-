// RavenEngine — AssetManager: UUID→Asset cache, async loads, artifact cache,
// hot reload, GPU upload hand-off and reference-count GC.
#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "raven/assets/AssetDatabase.h"
#include "raven/assets/Asset.h"
#include "raven/core/UUID.h"

namespace raven {

class AssetManager {
public:
    struct LoadResult {
        bool ok = false;
        std::string error;
        std::shared_ptr<Asset> asset;
    };

    static AssetManager& Instance();

    // assetsRoot: project "assets/" folder. libraryRoot: project "Library/" folder
    // (artifacts go to <libraryRoot>/Artifacts/<uuid>.rasset).
    bool Initialize(const std::filesystem::path& assetsRoot, const std::filesystem::path& libraryRoot,
                    std::string* error = nullptr);
    void Shutdown();
    bool IsInitialized() const { return m_Initialized; }

    AssetDatabase& Database() { return m_Database; }
    const AssetDatabase& Database() const { return m_Database; }
    const std::filesystem::path& LibraryRoot() const { return m_LibraryRoot; }

    // Called on the main thread with every asset that needs GPU resources.
    // The renderer registers this to build VkImage/VkBuffer/descriptor sets.
    using GpuUploadFn = std::function<void(const std::shared_ptr<Asset>&)>;
    void SetGpuUploadHandler(GpuUploadFn fn) { m_GpuUpload = std::move(fn); }

    // Cache lookup (nil when not loaded).
    std::shared_ptr<Asset> Find(const UUID& uuid) const;
    template <typename T>
    std::shared_ptr<T> FindAs(const UUID& uuid) const {
        auto asset = Find(uuid);
        return std::dynamic_pointer_cast<T>(std::move(asset));
    }

    LoadResult LoadImmediate(const UUID& uuid);                 // blocking (editor use)
    std::future<LoadResult> LoadAsync(const UUID& uuid);        // worker-thread import

    // Re-import from source, overwrite the artifact and hand a fresh asset to the
    // GPU upload handler (which replaces any previous GPU resource for this UUID).
    LoadResult ReloadAsset(const UUID& uuid);

    // Pins keep assets alive regardless of cache refcount (uses can pin for the
    // scene's lifetime to avoid GC churn).
    void Pin(const UUID& uuid) { m_Pinned.insert(uuid); }
    void Unpin(const UUID& uuid) { m_Pinned.erase(uuid); }
    bool IsPinned(const UUID& uuid) const { return m_Pinned.count(uuid) != 0; }

    // Drop cached assets whose only reference is the cache itself and are unpinned.
    size_t CollectGarbage();

    // Drain the async-completion queue; must be called from the main thread once per frame.
    void FlushPending();

    size_t CachedCount() const;
    std::vector<UUID> CachedUuids() const;

    std::filesystem::path ArtifactPathFor(const UUID& uuid) const;

private:
    AssetManager() = default;
    ~AssetManager();
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    LoadResult LoadInternal(const UUID& uuid, bool forceReimport);
    bool ArtifactIsFresh(const AssetDatabase::Entry& entry, const std::filesystem::path& artifactPath) const;

    AssetDatabase m_Database;
    std::filesystem::path m_LibraryRoot;
    GpuUploadFn m_GpuUpload;

    mutable std::mutex m_CacheMutex;
    std::unordered_map<UUID, std::shared_ptr<Asset>, UUIDHash> m_Cache;

    std::mutex m_PendingMutex;
    std::vector<std::shared_ptr<Asset>> m_PendingUploads;

    std::unordered_set<UUID, UUIDHash> m_Pinned;
    bool m_Initialized = false;
};

} // namespace raven
