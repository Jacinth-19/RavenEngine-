// RavenEngine — AssetDatabase: scans an assets root, assigns/loads UUIDs via
// .meta sidecars, maintains path<->UUID maps, reports dirty (changed) and
// orphaned (missing source) entries.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"

namespace raven {

class AssetDatabase {
public:
    struct Entry {
        UUID uuid;
        AssetType type = AssetType::Unknown;
        std::string relativePath;              // forward-slash, assets-root relative
        std::filesystem::path absolutePath;    // resolved against assets root
        bool isDirty = false;                  // source changed since last scan
        bool isOrphan = false;                 // .meta existed but source is gone
        std::filesystem::file_time_type lastWrite{};
    };

    AssetDatabase() = default;

    // Scans (or re-scans) the assets root. Missing .meta files are created.
    bool Initialize(const std::filesystem::path& assetsRoot, std::string* error = nullptr);
    bool Rescan(std::string* error = nullptr);

    const std::filesystem::path& AssetsRoot() const { return m_AssetsRoot; }

    const Entry* FindByPath(const std::string& relativePath) const;
    const Entry* FindByUUID(const UUID& uuid) const;
    std::vector<const Entry*> Entries() const;         // stable order (sorted by path)
    std::vector<const Entry*> EntriesOfType(AssetType type) const;

    // Marks `uuid` clean (used after a successful reimport).
    void MarkClean(const UUID& uuid);

    size_t OrphanCount() const { return m_Orphans.size(); }
    const std::vector<std::string>& Orphans() const { return m_Orphans; }

private:
    std::filesystem::path AbsoluteFor(const std::string& relativePath) const;
    void ScanInternal();

    std::filesystem::path m_AssetsRoot;
    std::unordered_map<std::string, Entry> m_ByPath;          // key: relativePath
    std::unordered_map<UUID, std::string, UUIDHash> m_PathByUUID;
    std::vector<std::string> m_Orphans;                       // meta paths without source
    bool m_Initialized = false;
};

} // namespace raven
