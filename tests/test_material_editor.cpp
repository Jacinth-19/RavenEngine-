// Editor ↔ importer contract (§6.3 material resource editor).
// The material editor rewrites the .rmat JSON in place and reimports; this test
// pins that the JSON shape the editor writes is exactly what MaterialImporter
// consumes — scalars, albedo/emissive arrays (4/3 components), and UUID texture
// references (the canonical .rmat form).
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "raven/assets/Asset.h"
#include "raven/assets/Importers.h"
#include "raven/core/UUID.h"

namespace fs = std::filesystem;
using namespace raven;

namespace {

const UUID kAlbedoUuid = UUID::Parse("a41c7b09-5e2d-4b8f-b3d6-2c8f1a0e91b7").value();
const UUID kNormalUuid = UUID::Parse("b52d8c1a-6f3e-4a9c-c4e7-3d9a2b1f02c8").value();

fs::path MakeTempRmat() {
    const fs::path path = fs::temp_directory_path() / "raven_test_material_editor_roundtrip.rmat";
    std::error_code ec;
    fs::remove(path, ec);
    return path;
}

bool WriteTextFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

} // namespace

RAVEN_TEST("material editor: JSON written by the editor roundtrips through MaterialImporter") {
    // Exactly what ApplyMaterialEdit persists: 4-component albedo/emissive arrays,
    // scalar metallic/roughness/normalScale, UUID texture refs.
    nlohmann::json material;
    material["albedo"] = {0.10f, 0.20f, 0.30f, 0.90f};
    material["emissive"] = {0.01f, 0.02f, 0.03f};
    material["metallic"] = 0.75f;
    material["roughness"] = 0.25f;
    material["normalScale"] = 1.5f;
    material["albedoTexture"] = kAlbedoUuid.ToString();
    material["normalTexture"] = kNormalUuid.ToString();

    const fs::path path = MakeTempRmat();
    REQUIRE(WriteTextFile(path, material.dump(2)));

    // No database: UUID references pass through the importer untouched.
    const ImportResult result = MaterialImporter::Import(path, nlohmann::json::object(), UUID::Generate(), nullptr);
    CHECK(result.ok);
    if (!result.ok) return;
    auto asset = std::dynamic_pointer_cast<MaterialAsset>(result.asset);
    CHECK(asset != nullptr);
    if (!asset) return;
    CHECK(asset->albedoTexture == kAlbedoUuid);
    CHECK(asset->normalTexture == kNormalUuid);
    CHECK(asset->metallic == 0.75f);
    CHECK(asset->roughness == 0.25f);
    CHECK(asset->normalScale == 1.5f);
    CHECK(asset->albedo.r == 0.10f);
    CHECK(asset->albedo.g == 0.20f);
    CHECK(asset->albedo.b == 0.30f);
    CHECK(asset->albedo.a == 0.90f);
    CHECK(asset->emissive.x == 0.01f);
    CHECK(asset->emissive.y == 0.02f);
    CHECK(asset->emissive.z == 0.03f);

    std::error_code ec;
    fs::remove(path, ec);
}

RAVEN_TEST("material editor: cleared texture references import as nil UUIDs") {
    nlohmann::json material;
    material["albedo"] = {0.5f, 0.5f, 0.5f, 1.0f};
    material["metallic"] = 0.0f;
    material["roughness"] = 0.6f;
    material["albedoTexture"] = "";     // editor "clear" writes the empty string
    material["normalTexture"] = "";

    const fs::path path = MakeTempRmat();
    REQUIRE(WriteTextFile(path, material.dump(2)));

    const ImportResult result = MaterialImporter::Import(path, nlohmann::json::object(), UUID::Generate(), nullptr);
    CHECK(result.ok);
    if (!result.ok) return;
    auto asset = std::dynamic_pointer_cast<MaterialAsset>(result.asset);
    CHECK(asset != nullptr);
    if (!asset) return;
    CHECK(asset->albedoTexture.IsNil());
    CHECK(asset->normalTexture.IsNil());

    std::error_code ec;
    fs::remove(path, ec);
}

RAVEN_TEST("material editor: path-form texture references still resolve against a database") {
    // Forward-compat: the importer also accepts assets-root-relative paths. The
    // editor writes UUIDs, but files authored by hand may use paths.
    nlohmann::json material;
    material["albedoTexture"] = "textures/sample_crate.png";
    material["normalTexture"] = "";

    const fs::path path = MakeTempRmat();
    REQUIRE(WriteTextFile(path, material.dump(2)));

    // No database provided: the path is reported as unresolvable but import succeeds.
    const ImportResult result = MaterialImporter::Import(path, nlohmann::json::object(), UUID::Generate(), nullptr);
    CHECK(result.ok);
    if (!result.ok) return;
    auto asset = std::dynamic_pointer_cast<MaterialAsset>(result.asset);
    CHECK(asset != nullptr);
    if (!asset) return;
    CHECK(asset->albedoTexture.IsNil());   // unresolved, not garbage

    std::error_code ec;
    fs::remove(path, ec);
}
