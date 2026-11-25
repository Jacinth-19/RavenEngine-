#include "raven/core/FileSystem.h"

#include <fstream>
#include <sstream>

namespace raven::fs {

bool Exists(const Path& path) { return std::filesystem::exists(path); }
bool IsDirectory(const Path& path) { std::error_code ec; return std::filesystem::is_directory(path, ec); }
bool IsRegularFile(const Path& path) { std::error_code ec; return std::filesystem::is_regular_file(path, ec); }

uint64_t FileSize(const Path& path) {
    std::error_code ec;
    auto s = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(s);
}

std::filesystem::file_time_type LastWriteTime(const Path& path) {
    std::error_code ec;
    return std::filesystem::last_write_time(path, ec);
}

std::optional<std::vector<uint8_t>> ReadBinary(const Path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (error) *error = "cannot open '" + path.string() + "'";
        return std::nullopt;
    }
    auto size = in.tellg();
    if (size < 0) {
        if (error) *error = "cannot stat '" + path.string() + "'";
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in && size > 0) {
        if (error) *error = "failed reading '" + path.string() + "'";
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::string> ReadText(const Path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open '" + path.string() + "'";
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool WriteCommon(const Path& path, const void* data, size_t size, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write '" + path.string() + "'";
        return false;
    }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) {
        if (error) *error = "failed writing '" + path.string() + "'";
        return false;
    }
    return true;
}

bool WriteBinary(const Path& path, const void* data, size_t size, std::string* error) {
    return WriteCommon(path, data, size, error);
}

bool WriteText(const Path& path, const std::string& text, std::string* error) {
    return WriteCommon(path, text.data(), text.size(), error);
}

bool EnsureDirectories(const Path& path, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool CopyFileTo(const Path& from, const Path& to, std::string* error) {
    std::error_code ec;
    if (to.has_parent_path()) std::filesystem::create_directories(to.parent_path(), ec);
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool RemoveFile(const Path& path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

std::string RelativePathString(const Path& root, const Path& child) {
    std::error_code ec;
    Path rel = std::filesystem::relative(std::filesystem::weakly_canonical(child, ec),
                                         std::filesystem::weakly_canonical(root, ec), ec);
    if (ec) rel = child.filename();
    return NormalizeSlashes(rel).string();
}

Path NormalizeSlashes(const Path& path) {
    std::string s = path.generic_string();
    return Path(s);
}

std::vector<Path> ListFilesRecursive(const Path& root) {
    std::vector<Path> files;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return files;
    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        const auto& entry = *it;
        if (it.depth() > 0) {
            const std::string dirName = entry.path().filename().string();
            if (entry.is_directory(ec) && (!dirName.empty() && (dirName[0] == '.' || dirName == "Library"))) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (entry.is_regular_file(ec)) files.push_back(entry.path());
    }
    return files;
}

} // namespace raven::fs
