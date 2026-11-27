// Asset pipeline: scan the sample project, import mesh/texture/material/audio artifacts,
// verify mip chains, texture dependency resolution and artifact caching.
//
// The asset manager is a process-wide singleton that initializes once, so the whole
// pipeline check runs as a single test against one temp copy of the sample project.
#include "test_framework.h"

#include <filesystem>
#include <memory>

#include "raven/assets/AssetManager.h"
#include "raven/assets/Asset.h"
#include "raven/core/UUID.h"

namespace fs = std::filesystem;
using namespace raven;

#ifndef RAVEN_SAMPLE_DIR
#define RAVEN_SAMPLE_DIR "sample"
#endif

namespace {

fs::path CreateTempProjectCopy() {
    const fs::path dst = fs::temp_directory_path() / "raven_test_sample_project";
    fs::remove_all(dst);
    fs::create_directories(dst);
    const fs::path src = fs::path(RAVEN_SAMPLE_DIR);
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        const fs::path rel = fs::relative(entry.path(), src);
        const fs::path target = dst / rel;
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
    return dst;
}

const UUID kMeshUuid = UUID::Parse("3f6b9c2a-1a7e-4c1d-9b2f-8d5a6e7f1023").value();
const UUID kAlbedoUuid = UUID::Parse("a41c7b09-5e2d-4b8f-b3d6-2c8f1a0e91b7").value();
const UUID kNormalUuid = UUID::Parse("b52d8c1a-6f3e-4a9c-c4e7-3d9a2b1f02c8").value();
const UUID kMaterialUuid = UUID::Parse("c63e9d2b-7a4f-4b0d-d5f8-4e0b3c2a13d9").value();
const UUID kAudioUuid = UUID::Parse("e8501f4d-9c6b-4d2f-f7b0-6a2d5e4c35f1").value();

} // namespace

RAVEN_TEST("assets: sample project imports end-to-end") {
    const fs::path project = CreateTempProjectCopy();
    AssetManager& mgr = AssetManager::Instance();
    std::string error;
    REQUIRE(mgr.Initialize(project / "assets", project / "Library", &error));

    // --- database indexes by the stable UUIDs from the .meta files ---
    const auto* meshEntry = mgr.Database().FindByUUID(kMeshUuid);
    CHECK(meshEntry != nullptr);
    if (meshEntry) CHECK(meshEntry->type == AssetType::Mesh);
    const auto* texEntry = mgr.Database().FindByUUID(kAlbedoUuid);
    CHECK(texEntry != nullptr);
    if (texEntry) CHECK(texEntry->type == AssetType::Texture);

    // --- mesh import: geometry + bounds ---
    auto meshResult = mgr.LoadImmediate(kMeshUuid);
    CHECK(meshResult.ok);
    auto mesh = std::dynamic_pointer_cast<MeshAsset>(meshResult.asset);
    CHECK(mesh != nullptr);
    if (mesh) {
        CHECK_EQ(mesh->triangleCount, 12u);
        CHECK_EQ(mesh->vertices.size(), 24u);
        CHECK_EQ(mesh->indices.size(), 36u);
        CHECK(mesh->boundsMin.x > -1.01f && mesh->boundsMin.x < -0.99f);
        CHECK(mesh->boundsMax.x > 0.99f && mesh->boundsMax.x < 1.01f);
    }
    CHECK(fs::exists(mgr.ArtifactPathFor(kMeshUuid)));

    // --- texture import: mip chain 256→1 ---
    auto texResult = mgr.LoadImmediate(kAlbedoUuid);
    CHECK(texResult.ok);
    auto tex = std::dynamic_pointer_cast<TextureAsset>(texResult.asset);
    CHECK(tex != nullptr);
    if (tex) {
        CHECK_EQ(tex->width, 256u);
        CHECK_EQ(tex->height, 256u);
        CHECK_EQ(tex->mips.size(), 9u);
        CHECK_EQ(tex->mips.back().width, 1u);
        CHECK(tex->isSrgb);
    }

    // --- normal map imports as linear (not sRGB) ---
    auto normalResult = mgr.LoadImmediate(kNormalUuid);
    CHECK(normalResult.ok);
    auto normal = std::dynamic_pointer_cast<TextureAsset>(normalResult.asset);
    CHECK(normal != nullptr);
    if (normal) CHECK(!normal->isSrgb);

    // --- material resolves its texture references ---
    auto matResult = mgr.LoadImmediate(kMaterialUuid);
    CHECK(matResult.ok);
    auto material = std::dynamic_pointer_cast<MaterialAsset>(matResult.asset);
    CHECK(material != nullptr);
    if (material) {
        CHECK(material->albedoTexture == kAlbedoUuid);
        CHECK(material->normalTexture == kNormalUuid);
        CHECK(material->roughness > 0.0f && material->roughness < 1.0f);
    }

    // --- audio keeps its payload ---
    auto audioResult = mgr.LoadImmediate(kAudioUuid);
    CHECK(audioResult.ok);
    auto audio = std::dynamic_pointer_cast<AudioAsset>(audioResult.asset);
    CHECK(audio != nullptr);
    if (audio) {
        CHECK(audio->bytes.size() > 100000);
        CHECK(audio->format == "wav");
    }

    // --- cache: second load returns the same asset pointer ---
    auto cachedMesh = mgr.LoadImmediate(kMeshUuid);
    CHECK(cachedMesh.asset == meshResult.asset);

    // keep the temp copy: the vulkan smoke test reuses the same stable path
}
