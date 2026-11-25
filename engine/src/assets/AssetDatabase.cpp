#include <utility>
#include "raven/assets/AssetDatabase.h"
#include "raven/assets/AssetMeta.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"

#include <algorithm>
#include <unordered_set>

namespace raven {

bool AssetDatabase::Initialize(const std::filesystem::path& assetsRoot, std::string* error) {
    std::error_code ec;
    if (!fs::IsDirectory(assetsRoot)) {
        if (!fs::EnsureDirectories(assetsRoot, error)) return false;
    }
    m_AssetsRoot = std::filesystem::weakly_canonical(assetsRoot, ec);
    ScanInternal();
    // First scan establishes the baseline — nothing is "changed" yet.
    for (auto& [rel, entry] : m_ByPath) entry.isDirty = false;
    m_Initialized = true;
    RAVEN_LOG_INFO("AssetDatabase: %zu assets, %zu orphaned metas under %s",
                   m_ByPath.size(), m_Orphans.size(), m_AssetsRoot.string().c_str());
    return true;
}

bool AssetDatabase::Rescan(std::string* error) {
    if (!m_Initialized) return Initialize(m_AssetsRoot, error);
    ScanInternal();
    return true;
}

void AssetDatabase::ScanInternal() {
    // Snapshot of previous state to compute dirty flags.
    std::unordered_map<std::string, std::filesystem::file_time_type> previousWrites;
    previousWrites.reserve(m_ByPath.size());
    for (const auto& [rel, entry] : m_ByPath) previousWrites.emplace(rel, entry.lastWrite);

    m_ByPath.clear();
    m_PathByUUID.clear();
    m_Orphans.clear();

    auto files = fs::ListFilesRecursive(m_AssetsRoot);
    std::unordered_set<std::string> sourceFiles;
    sourceFiles.reserve(files.size());

    for (const auto& absolute : files) {
        std::string filename = absolute.filename().string();
        if (filename.size() >= 5 && filename.compare(filename.size() - 5, 5, ".meta") == 0) continue;

        std::string relative = fs::RelativePathString(m_AssetsRoot, absolute);
        sourceFiles.insert(relative);

        AssetType type = AssetTypeFromExtension(absolute.string());
        if (type == AssetType::Unknown) continue;  // not an asset type we track yet

        // Ensure a .meta sidecar exists with a stable UUID.
        fs::Path metaPath = MetaPathFor(absolute);
        std::optional<AssetMeta> meta;
        std::error_code ec;
        if (fs::IsRegularFile(metaPath)) {
            std::string metaError;
            meta = ReadMeta(metaPath, &metaError);
            if (!meta) {
                RAVEN_LOG_WARN("AssetDatabase: unreadable meta %s (%s) — regenerating", metaPath.string().c_str(), metaError.c_str());
            }
        }
        if (!meta) {
            AssetMeta fresh;
            fresh.uuid = UUID::Generate();
            fresh.type = type;
            fresh.relativePath = relative;
            fresh.importSettings = DefaultImportSettings(type);
            std::string metaError;
            if (!WriteMeta(metaPath, fresh, &metaError)) {
                RAVEN_LOG_ERROR("AssetDatabase: cannot write meta %s: %s", metaPath.string().c_str(), metaError.c_str());
                continue;
            }
            meta = fresh;
            RAVEN_LOG_INFO("AssetDatabase: registered %s -> %s", relative.c_str(), meta->uuid.ToString().c_str());
        } else if (meta->type != type) {
            RAVEN_LOG_WARN("AssetDatabase: %s has stale type '%s' (expected '%s'); keeping UUID",
                           relative.c_str(), AssetTypeToString(meta->type), AssetTypeToString(type));
            meta->type = type;
        }

        Entry entry;
        entry.uuid = meta->uuid;
        entry.type = type;
        entry.relativePath = relative;
        entry.absolutePath = absolute;
        entry.lastWrite = fs::LastWriteTime(absolute);
        auto prev = previousWrites.find(relative);
        entry.isDirty = (prev == previousWrites.end()) || (prev->second != entry.lastWrite);

        m_PathByUUID.emplace(entry.uuid, relative);
        m_ByPath.emplace(relative, std::move(entry));
    }

    // Discover orphan .meta files (source gone).
    std::vector<fs::Path> allFiles = files;
    std::sort(allFiles.begin(), allFiles.end());
    for (const auto& file : allFiles) {
        std::string filename = file.filename().string();
        if (filename.size() >= 5 && filename.compare(filename.size() - 5, 5, ".meta") == 0) {
            fs::Path source = file;
            std::string s = file.string();
            s.erase(s.size() - 5);
            source = fs::Path(s);
            std::error_code ec2;
            if (!fs::Exists(source)) {
                m_Orphans.push_back(fs::RelativePathString(m_AssetsRoot, file));
            }
        }
    }
}

std::filesystem::path AssetDatabase::AbsoluteFor(const std::string& relativePath) const {
    return m_AssetsRoot / fs::Path(relativePath);
}

const AssetDatabase::Entry* AssetDatabase::FindByPath(const std::string& relativePath) const {
    auto it = m_ByPath.find(relativePath);
    return it == m_ByPath.end() ? nullptr : &it->second;
}

const AssetDatabase::Entry* AssetDatabase::FindByUUID(const UUID& uuid) const {
    auto it = m_PathByUUID.find(uuid);
    if (it == m_PathByUUID.end()) return nullptr;
    return FindByPath(it->second);
}

std::vector<const AssetDatabase::Entry*> AssetDatabase::Entries() const {
    std::vector<const Entry*> entries;
    entries.reserve(m_ByPath.size());
    for (const auto& [rel, entry] : m_ByPath) entries.push_back(&entry);
    std::sort(entries.begin(), entries.end(),
              [](const Entry* a, const Entry* b) { return a->relativePath < b->relativePath; });
    return entries;
}

std::vector<const AssetDatabase::Entry*> AssetDatabase::EntriesOfType(AssetType type) const {
    std::vector<const Entry*> out;
    for (const auto* e : Entries())
        if (e->type == type) out.push_back(e);
    return out;
}

void AssetDatabase::MarkClean(const UUID& uuid) {
    auto it = m_PathByUUID.find(uuid);
    if (it == m_PathByUUID.end()) return;
    auto pathIt = m_ByPath.find(it->second);
    if (pathIt != m_ByPath.end()) pathIt->second.isDirty = false;
}

} // namespace raven
