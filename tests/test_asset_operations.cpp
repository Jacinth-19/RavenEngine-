// Phase 3 (§10.1): filesystem operations behind the ProjectPanel context menu.
// AssetOperations.cpp is compiled straight into the test binary (it is imgui-free).
#include "test_framework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "editor/core/AssetOperations.h"
#include "raven/assets/AssetMeta.h"   // MetaPathFor

using namespace raven;
using namespace raven::editor;

namespace {

struct TempProject {
    std::filesystem::path root;
    TempProject() {
        root = std::filesystem::temp_directory_path() / ("raven_asset_ops_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "textures", ec);
        std::filesystem::create_directories(root / "materials", ec);
        WriteFile(root / "textures" / "crate.png", "fake png bytes");
        WriteFile(root / "textures" / "crate.png.meta", "{\"uuid\": \"11111111-1111-1111-1111-111111111111\", \"type\": \"texture\"}");
        WriteFile(root / "materials" / "existing.rmat", "{\"roughness\": 0.4}");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    static void WriteFile(const std::filesystem::path& path, const std::string& content) {
        std::ofstream out(path, std::ios::trunc);
        out << content;
    }
};

} // namespace

RAVEN_TEST("assets: unique child path picks first free variant") {
    TempProject p;
    const std::filesystem::path first = UniqueChildPath(p.root / "materials", "existing", ".rmat");
    CHECK(first.filename() == "existing 2.rmat");   // existing.rmat taken
    const std::filesystem::path fresh = UniqueChildPath(p.root / "materials", "brand-new", ".rmat");
    CHECK(fresh.filename() == "brand-new.rmat");
}

RAVEN_TEST("assets: create folder unique-ifies and rejects unsafe names") {
    TempProject p;
    const AssetOpResult folder = CreateFolder(p.root, "textures");   // already exists
    CHECK(folder.ok);
    CHECK(folder.path.filename() == "textures 2");
    const AssetOpResult bad = CreateFolder(p.root, "../escape");
    CHECK(!bad.ok);
    const AssetOpResult slash = CreateFolder(p.root, "a/b");
    CHECK(!slash.ok);
}

RAVEN_TEST("assets: create material writes a parseable .rmat template") {
    TempProject p;
    const AssetOpResult result = CreateMaterialAsset(p.root / "materials", "stone");
    CHECK(result.ok);
    CHECK(std::filesystem::exists(result.path));
    std::ifstream in(result.path);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("\"roughness\"") != std::string::npos);
    CHECK(body.find("\"albedo\"") != std::string::npos);
}

RAVEN_TEST("assets: rename moves the file and its .meta sidecar together") {
    TempProject p;
    const std::filesystem::path source = p.root / "textures" / "crate.png";
    const AssetOpResult renamed = RenameAsset(source, "crate_renamed.png");
    CHECK(renamed.ok);
    CHECK(std::filesystem::exists(p.root / "textures" / "crate_renamed.png"));
    CHECK(std::filesystem::exists(p.root / "textures" / "crate_renamed.png.meta"));
    CHECK(!std::filesystem::exists(source));
    CHECK(!std::filesystem::exists(source.string() + ".meta") || !std::filesystem::exists(MetaPathFor(source)));
    TempProject::WriteFile(p.root / "textures" / "other.png", "other");
    const AssetOpResult clash = RenameAsset(p.root / "textures" / "crate_renamed.png", "other.png");   // target exists
    CHECK(!clash.ok);
    const AssetOpResult unsafe = RenameAsset(p.root / "textures" / "crate_renamed.png", "a/b.png");
    CHECK(!unsafe.ok);
}

RAVEN_TEST("assets: duplicate does NOT copy the .meta sidecar (fresh UUID on rescan)") {
    TempProject p;
    const AssetOpResult dup = DuplicateAsset(p.root / "textures" / "crate.png");
    CHECK(dup.ok);
    CHECK(dup.path.filename() == "crate copy.png");
    CHECK(std::filesystem::exists(dup.path));
    CHECK(!std::filesystem::exists(MetaPathFor(dup.path)));   // no sidecar → DB mints a fresh UUID
}

RAVEN_TEST("assets: duplicate unique-ifies when a copy already exists") {
    TempProject p;
    const AssetOpResult first = DuplicateAsset(p.root / "textures" / "crate.png");
    CHECK(first.ok);
    const AssetOpResult second = DuplicateAsset(p.root / "textures" / "crate.png");
    CHECK(second.ok);
    CHECK(second.path.filename() == "crate copy 2.png");
}

RAVEN_TEST("assets: delete removes asset and sidecar; missing sidecar is fine") {
    TempProject p;
    const AssetOpResult del = DeleteAsset(p.root / "textures" / "crate.png");
    CHECK(del.ok);
    CHECK(!std::filesystem::exists(p.root / "textures" / "crate.png"));
    CHECK(!std::filesystem::exists(p.root / "textures" / "crate.png.meta"));
    // The material has no sidecar — delete must still succeed.
    const AssetOpResult del2 = DeleteAsset(p.root / "materials" / "existing.rmat");
    CHECK(del2.ok);
    CHECK(!std::filesystem::exists(p.root / "materials" / "existing.rmat"));
    const AssetOpResult missing = DeleteAsset(p.root / "no-such-file.png");
    CHECK(!missing.ok);
}

RAVEN_TEST("assets: delete on a folder removes the tree recursively") {
    TempProject p;
    const AssetOpResult del = DeleteAsset(p.root / "textures");
    CHECK(del.ok);
    CHECK(!std::filesystem::exists(p.root / "textures"));
}
