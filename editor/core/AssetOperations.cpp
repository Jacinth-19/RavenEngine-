#include "editor/core/AssetOperations.h"

#include <cstdio>
#include <fstream>
#include <system_error>

#include "raven/assets/AssetMeta.h"   // MetaPathFor
#include "raven/core/Logger.h"

namespace raven::editor {

namespace {

bool IsSafeName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
    return true;
}

AssetOpResult Fail(std::string message) {
    RAVEN_LOG_ERROR("Assets: %s", message.c_str());
    return AssetOpResult{false, std::move(message), {}};
}

} // namespace

std::filesystem::path UniqueChildPath(const std::filesystem::path& parentDir, const std::string& stem, const std::string& extension) {
    std::error_code ec;
    for (int counter = 1;; ++counter) {
        const std::string suffix = counter == 1 ? std::string() : " " + std::to_string(counter);
        const std::filesystem::path candidate = parentDir / (stem + suffix + extension);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
        if (counter > 999) return candidate;   // give up pathologically; caller overwrites nothing
    }
}

AssetOpResult CreateFolder(const std::filesystem::path& parentDir, const std::string& requestedName) {
    if (!IsSafeName(requestedName)) return Fail("invalid folder name '" + requestedName + "'");
    const std::filesystem::path target = UniqueChildPath(parentDir, requestedName, "");
    std::error_code ec;
    if (!std::filesystem::create_directory(target, ec)) return Fail("cannot create folder " + target.string() + " (" + ec.message() + ")");
    RAVEN_LOG_INFO("Assets: created folder %s", target.string().c_str());
    return AssetOpResult{true, {}, target};
}

AssetOpResult CreateMaterialAsset(const std::filesystem::path& parentDir, const std::string& requestedName) {
    if (!IsSafeName(requestedName)) return Fail("invalid material name '" + requestedName + "'");
    const std::string stem = requestedName.size() > 6 && requestedName.substr(requestedName.size() - 6) == ".rmat"
                                 ? requestedName.substr(0, requestedName.size() - 6)
                                 : requestedName;
    const std::filesystem::path target = UniqueChildPath(parentDir, stem, ".rmat");
    static const char kTemplate[] =
        "{\n"
        "  \"albedo\": [0.8, 0.8, 0.8, 1.0],\n"
        "  \"metallic\": 0.0,\n"
        "  \"roughness\": 0.5,\n"
        "  \"albedoTexture\": null,\n"
        "  \"normalTexture\": null,\n"
        "  \"emissive\": [0.0, 0.0, 0.0],\n"
        "  \"normalScale\": 1.0\n"
        "}\n";
    std::ofstream out(target, std::ios::trunc);
    if (!out) return Fail("cannot write material " + target.string());
    out << kTemplate;
    out.close();
    RAVEN_LOG_INFO("Assets: created material %s", target.string().c_str());
    return AssetOpResult{true, {}, target};
}

AssetOpResult RenameAsset(const std::filesystem::path& path, const std::string& newName) {
    if (!IsSafeName(newName)) return Fail("invalid name '" + newName + "'");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return Fail("no such asset: " + path.string());
    const std::filesystem::path target = path.parent_path() / newName;
    if (target == path) return AssetOpResult{true, {}, target};
    if (std::filesystem::exists(target, ec)) return Fail("'" + newName + "' already exists");

    const std::filesystem::path meta = MetaPathFor(path);
    const std::filesystem::path metaTarget = MetaPathFor(target);
    const bool hadMeta = std::filesystem::exists(meta, ec);

    std::filesystem::rename(path, target, ec);
    if (ec) return Fail("rename failed: " + ec.message());
    if (hadMeta) {
        std::error_code metaEc;
        std::filesystem::rename(meta, metaTarget, metaEc);
        if (metaEc) RAVEN_LOG_WARN("Assets: renamed %s but its .meta sidecar could not be moved (%s) — rescan mints a fresh UUID",
                                    path.filename().string().c_str(), metaEc.message().c_str());
    }
    RAVEN_LOG_INFO("Assets: renamed %s -> %s", path.filename().string().c_str(), newName.c_str());
    return AssetOpResult{true, {}, target};
}

AssetOpResult DuplicateAsset(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return Fail("cannot duplicate (not a file): " + path.string());
    const std::filesystem::path target = UniqueChildPath(path.parent_path(), path.stem().string() + " copy", path.extension().string());
    std::filesystem::copy_file(path, target, std::filesystem::copy_options::none, ec);
    if (ec) return Fail("duplicate failed: " + ec.message());
    // Intentionally no .meta copy — the rescan registers the duplicate with a fresh UUID.
    RAVEN_LOG_INFO("Assets: duplicated %s -> %s", path.filename().string().c_str(), target.filename().string().c_str());
    return AssetOpResult{true, {}, target};
}

AssetOpResult DeleteAsset(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return Fail("no such asset: " + path.string());
    const bool isDir = std::filesystem::is_directory(path, ec);
    const std::filesystem::path meta = MetaPathFor(path);   // only meaningful for files

    if (isDir) std::filesystem::remove_all(path, ec);
    else std::filesystem::remove(path, ec);
    if (ec) return Fail("delete failed: " + ec.message());

    if (!isDir) {
        std::error_code metaEc;
        std::filesystem::remove(meta, metaEc);   // missing meta is fine
    }
    RAVEN_LOG_INFO("Assets: deleted %s%s", path.filename().string().c_str(), isDir ? " (folder)" : "");
    return AssetOpResult{true, {}, {}};
}

bool RevealInFileManager(const std::filesystem::path& path) {
    std::string command;
#if defined(_WIN32)
    command = "explorer /select,\"" + path.string() + "\"";
#elif defined(__APPLE__)
    command = "open -R \"" + path.string() + "\"";
#else
    command = "xdg-open \"" + path.parent_path().string() + "\" >/dev/null 2>&1 &";
#endif
    RAVEN_LOG_INFO("Assets: reveal %s", path.string().c_str());
    return std::system(command.c_str()) == 0;
}

} // namespace raven::editor
