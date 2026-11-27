#include <utility>
#include "raven/assets/AssetManager.h"

#include <algorithm>

#include "raven/assets/Artifact.h"
#include "raven/assets/BcnCodec.h"
#include "raven/assets/Importers.h"
#include "raven/assets/AssetMeta.h"
#include "raven/core/FileSystem.h"
#include "raven/core/JobSystem.h"
#include "raven/core/Logger.h"

namespace raven {

AssetManager& AssetManager::Instance() {
    static AssetManager instance;
    return instance;
}

AssetManager::~AssetManager() {
    Shutdown();
}

bool AssetManager::Initialize(const std::filesystem::path& assetsRoot, const std::filesystem::path& libraryRoot,
                              std::string* error) {
    if (m_Initialized) return true;
    if (!m_Database.Initialize(assetsRoot, error)) return false;
    m_LibraryRoot = libraryRoot;
    std::string libError;
    if (!fs::EnsureDirectories(m_LibraryRoot / "Artifacts", &libError)) {
        if (error) *error = libError;
        return false;
    }
    m_Initialized = true;
    JobSystem::Instance().Initialize();  // no-op when already running
    return true;
}

void AssetManager::Shutdown() {
    if (!m_Initialized) return;
    FlushPending();
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        m_Cache.clear();
    }
    m_Pinned.clear();
    m_Initialized = false;
}

std::filesystem::path AssetManager::ArtifactPathFor(const UUID& uuid) const {
    return m_LibraryRoot / "Artifacts" / (uuid.ToString() + ".rasset");
}

std::shared_ptr<Asset> AssetManager::Find(const UUID& uuid) const {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    auto it = m_Cache.find(uuid);
    return it == m_Cache.end() ? nullptr : it->second;
}

bool AssetManager::ArtifactIsFresh(const AssetDatabase::Entry& entry, const std::filesystem::path& artifactPath) const {
    std::error_code ec;
    if (!fs::IsRegularFile(artifactPath)) return false;
    auto artifactTime = fs::LastWriteTime(artifactPath);
    return artifactTime >= entry.lastWrite;
}

AssetManager::LoadResult AssetManager::LoadInternal(const UUID& uuid, bool forceReimport) {
    LoadResult result;
    const AssetDatabase::Entry* entry = m_Database.FindByUUID(uuid);
    if (!entry) {
        result.error = "asset not registered in database: " + uuid.ToString();
        return result;
    }

    const std::filesystem::path artifactPath = ArtifactPathFor(uuid);
    bool artifactOk = !forceReimport && ArtifactIsFresh(*entry, artifactPath);

    if (artifactOk) {
        auto data = ReadRasset(artifactPath, &result.error);
        if (data && data->type == entry->type) {
            if (auto asset = DeserializeAsset(entry->type, data->payload, uuid)) {
                result.ok = true;
                result.asset = std::move(asset);
                return result;
            }
            result.error = "failed to deserialize artifact payload";
        } else if (!data) {
            RAVEN_LOG_WARN("AssetManager: artifact unreadable (%s); re-importing %s",
                           result.error.c_str(), entry->relativePath.c_str());
        }
    }

    // Import from source.
    std::string metaError;
    std::string metaText;
    std::optional<AssetMeta> meta;
    if (auto text = fs::ReadText(MetaPathFor(entry->absolutePath))) metaText = *text;
    auto metaJson = nlohmann::json::parse(metaText, nullptr, false);
    if (!metaJson.is_discarded()) {
        auto parsed = AssetMeta::FromJson(metaJson, &metaError);
        if (parsed) meta = std::move(*parsed);
    }
    nlohmann::json settings = meta ? meta->importSettings : DefaultImportSettings(entry->type);
    // BCn artifacts land next to Library/Artifacts; importers stay library-root-agnostic.
    if (entry->type == AssetType::Texture && BcnFormatFromSetting(settings))
        settings["bc7CacheDir"] = (m_LibraryRoot / "bc7").string();

    RAVEN_LOG_DEBUG("AssetManager: importing %s", entry->relativePath.c_str());
    ImportResult imported = ImportAsset(entry->type, entry->absolutePath, settings, uuid, &m_Database);
    if (!imported.ok) {
        result.error = "import failed for " + entry->relativePath + ": " + imported.error;
        return result;
    }

    std::string writeError;
    if (!WriteRasset(artifactPath, entry->type, uuid, imported.payload, &writeError)) {
        RAVEN_LOG_WARN("AssetManager: could not write artifact %s: %s", artifactPath.string().c_str(), writeError.c_str());
    }
    m_Database.MarkClean(uuid);

    result.ok = true;
    result.asset = std::move(imported.asset);
    return result;
}

AssetManager::LoadResult AssetManager::LoadImmediate(const UUID& uuid) {
    if (auto cached = Find(uuid)) {
        LoadResult result;
        result.ok = true;
        result.asset = std::move(cached);
        return result;
    }
    LoadResult result = LoadInternal(uuid, /*forceReimport=*/false);
    if (!result.ok) {
        RAVEN_LOG_ERROR("AssetManager: %s", result.error.c_str());
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        m_Cache.emplace(result.asset->uuid, result.asset);
    }
    if (m_GpuUpload) {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        m_PendingUploads.push_back(result.asset);
    }
    return result;
}

std::future<AssetManager::LoadResult> AssetManager::LoadAsync(const UUID& uuid) {
    return JobSystem::Instance().Submit([this, uuid]() -> LoadResult {
        LoadResult result = LoadInternal(uuid, /*forceReimport=*/false);
        if (!result.ok) return result;
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            m_Cache.emplace(result.asset->uuid, result.asset);
        }
        if (m_GpuUpload) {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            m_PendingUploads.push_back(result.asset);
        }
        return result;
    });
}

std::future<AssetManager::LoadResult> AssetManager::ReloadAsync(const UUID& uuid) {
    return JobSystem::Instance().Submit([this, uuid]() -> LoadResult {
        LoadResult result = LoadInternal(uuid, /*forceReimport=*/true);
        if (!result.ok) return result;
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            m_Cache[result.asset->uuid] = result.asset;  // replace
        }
        if (m_GpuUpload) {
            std::lock_guard<std::mutex> lock(m_PendingMutex);
            m_PendingUploads.push_back(result.asset);     // renderer swaps resources in-place
        }
        return result;
    });
}

AssetManager::LoadResult AssetManager::ReloadAsset(const UUID& uuid) {
    LoadResult result = LoadInternal(uuid, /*forceReimport=*/true);
    if (!result.ok) {
        RAVEN_LOG_ERROR("AssetManager: reload failed: %s", result.error.c_str());
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        m_Cache[result.asset->uuid] = result.asset;  // replace
    }
    if (m_GpuUpload) {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        m_PendingUploads.push_back(result.asset);     // renderer swaps resources in-place
    }
    RAVEN_LOG_INFO("AssetManager: hot-reloaded %s", uuid.ToString().c_str());
    return result;
}

void AssetManager::FlushPending() {
    std::vector<std::shared_ptr<Asset>> pending;
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        pending.swap(m_PendingUploads);
    }
    if (m_GpuUpload) {
        for (const auto& asset : pending) m_GpuUpload(asset);
    }
}

size_t AssetManager::CollectGarbage() {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    size_t removed = 0;
    for (auto it = m_Cache.begin(); it != m_Cache.end();) {
        if (!m_Pinned.count(it->first) && it->second.use_count() == 1) {
            it = m_Cache.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) RAVEN_LOG_INFO("AssetManager: GC unloaded %zu unused asset(s)", removed);
    return removed;
}

size_t AssetManager::CachedCount() const {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    return m_Cache.size();
}

std::vector<UUID> AssetManager::CachedUuids() const {
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    std::vector<UUID> out;
    out.reserve(m_Cache.size());
    for (const auto& [uuid, _] : m_Cache) out.push_back(uuid);
    return out;
}

} // namespace raven
