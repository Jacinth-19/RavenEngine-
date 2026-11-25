// RavenEngine — thin helpers over std::filesystem with consistent error reporting.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace raven::fs {

using Path = std::filesystem::path;

bool Exists(const Path& path);
bool IsDirectory(const Path& path);
bool IsRegularFile(const Path& path);
uint64_t FileSize(const Path& path);             // 0 when missing
std::filesystem::file_time_type LastWriteTime(const Path& path);

std::optional<std::vector<uint8_t>> ReadBinary(const Path& path, std::string* error = nullptr);
std::optional<std::string> ReadText(const Path& path, std::string* error = nullptr);
bool WriteBinary(const Path& path, const void* data, size_t size, std::string* error = nullptr);
bool WriteText(const Path& path, const std::string& text, std::string* error = nullptr);
bool EnsureDirectories(const Path& path, std::string* error = nullptr);
bool CopyFileTo(const Path& from, const Path& to, std::string* error = nullptr);
bool RemoveFile(const Path& path);

// "assets/meshes/cube.obj" style: forward slashes, no leading slash,
// relative to the given root (absolute and weakly-canonical on both sides).
std::string RelativePathString(const Path& root, const Path& child);
Path NormalizeSlashes(const Path& path);

// Recursive listing of regular files (skips hidden dirs like ".git" and Library/).
std::vector<Path> ListFilesRecursive(const Path& root);

} // namespace raven::fs
