// RavenEditor — filesystem operations behind the ProjectPanel context menu (§10.1).
//
// These are deliberately NOT undoable engine commands: they mutate the *source tree*
// (create/rename/duplicate/delete bytes on disk), not scene state, and hard delete is
// spec'd in §10.1 as permanent. The panel still logs every op and the asset DB is
// rescanned after each one, so the browser and the DB never disagree for long.
//
// Rename/duplicate move the `<file>.meta` sidecar along. Duplicate mints a fresh UUID
// by *not copying* the .meta — the next rescan registers the copy with a new UUID
// (1:1 with Luth's duplicate semantics).
#pragma once

#include <filesystem>
#include <string>

namespace raven::editor {

struct AssetOpResult {
    bool ok = false;
    std::string error;
    std::filesystem::path path;   // resulting path on success (created/renamed/duplicated)
};

// "New Folder" — unique-ifies the name (folder, folder 2, folder 3, …).
AssetOpResult CreateFolder(const std::filesystem::path& parentDir, const std::string& requestedName);

// Writes a neutral .rmat template (linear albedo, 0.5 roughness, no textures).
AssetOpResult CreateMaterialAsset(const std::filesystem::path& parentDir, const std::string& requestedName);

// Renames `path` to `newName` within the same directory; moves the .meta sidecar too.
AssetOpResult RenameAsset(const std::filesystem::path& path, const std::string& newName);

// Copies `path` next to itself as "<stem> copy<extn>" (unique-ified). .meta not copied.
AssetOpResult DuplicateAsset(const std::filesystem::path& path);

// Permanent delete of the asset and its .meta sidecar. No trash (§10.1, documented).
AssetOpResult DeleteAsset(const std::filesystem::path& path);

// Opens the containing folder in the OS file manager (xdg-open / open / explorer).
bool RevealInFileManager(const std::filesystem::path& path);

// "name", "name 2", "name 3"… first variant that does not exist under parentDir.
std::filesystem::path UniqueChildPath(const std::filesystem::path& parentDir, const std::string& stem, const std::string& extension);

} // namespace raven::editor
