// Phase 3 (§10.3/§10.4): name-rule scoring + texture remap persistence.
#include "test_framework.h"

#include <filesystem>
#include <chrono>
#include <fstream>
#include <string>
#include <system_error>

#include "raven/assets/AssetMeta.h"
#include "raven/assets/TextureNameRules.h"

#include "editor/core/TextureRemap.h"

using namespace raven;
using namespace raven::editor;

RAVEN_TEST("name rules: trailing whole-word token scores highest") {
    CHECK(ScoreTextureForSlot("crate_d", TextureSlot::Diffuse) >= 100);
    CHECK(ScoreTextureForSlot("crate_n", TextureSlot::Normal) >= 100);
    CHECK(ScoreTextureForSlot("crate_roughness", TextureSlot::Roughness) >= 100);
    CHECK(ScoreTextureForSlot("crate_m", TextureSlot::Metalness) >= 100);
    CHECK(ScoreTextureForSlot("crate_ao", TextureSlot::Occlusion) >= 100);
}

RAVEN_TEST("name rules: single-letter aliases require a word boundary") {
    CHECK(ScoreTextureForSlot("barn_wood", TextureSlot::Metalness) == 0);   // 'm' substring must not match
    CHECK(ScoreTextureForSlot("wood_r", TextureSlot::Roughness) >= 100);    // boundary ok
    CHECK(ScoreTextureForSlot("wood_r", TextureSlot::Diffuse) == 0);
}

RAVEN_TEST("name rules: a clearer competing slot suppresses the pick (negative score)") {
    // "albedo_normal" is a normal map, not a diffuse one — diffuse must not claim it.
    CHECK(ScoreTextureForSlot("albedo_normal", TextureSlot::Diffuse) < 0);
    CHECK(ScoreTextureForSlot("albedo_normal", TextureSlot::Normal) >= 100);
    // ...but own-slot wins when it is the trailing token.
    CHECK(ScoreTextureForSlot("normal_albedo", TextureSlot::Diffuse) > 0);
}

RAVEN_TEST("name rules: ranking orders candidates and drops zero-score stems") {
    const std::vector<std::string> stems{"wood", "wood_d", "wood_n", "stone_d", "metal_d"};
    const auto ranked = RankCandidates(TextureSlot::Diffuse, stems);
    REQUIRE(ranked.size() == 3);
    CHECK(ranked[0].stem == "metal_d");   // tie in score -> alphabetical
    CHECK(ranked[1].stem == "stone_d");
    CHECK(ranked[2].stem == "wood_d");
    CHECK(IsAmbiguous(ranked, 15));       // all tied at the same score

    const auto clear = RankCandidates(TextureSlot::Normal, stems);
    REQUIRE(clear.size() == 1);
    CHECK(clear[0].stem == "wood_n");
    CHECK(!IsAmbiguous(clear, 15));
}

RAVEN_TEST("remap json: decisions round trip with skips preserved") {
    std::vector<RemapDecision> in;
    in.push_back({TextureSlot::Diffuse, "textures/crate_albedo.png"});
    in.push_back({TextureSlot::Normal, "textures/crate_normal.png"});
    in.push_back({TextureSlot::Occlusion, ""});   // explicitly skipped

    const nlohmann::json json = RemapToJson(in);
    CHECK(json["diffuse"] == "textures/crate_albedo.png");
    CHECK(json["normal"] == "textures/crate_normal.png");
    CHECK(json["occlusion"] == "");

    std::vector<std::string> unknown;
    const auto out = RemapFromJson(json, &unknown);
    REQUIRE(out.size() == 3);
    CHECK(unknown.empty());
    auto bySlot = DecisionsBySlot(json);
    REQUIRE(bySlot.size() == 3);
    bool sawSkip = false, sawDiffuse = false;
    for (const auto& [slot, path] : bySlot) {
        if (slot == TextureSlot::Occlusion) { sawSkip = path.empty(); }
        if (slot == TextureSlot::Diffuse) { sawDiffuse = path == "textures/crate_albedo.png"; }
    }
    CHECK(sawSkip);
    CHECK(sawDiffuse);
}

RAVEN_TEST("remap json: unknown slot keys are reported, not crashed on") {
    nlohmann::json json = {{"diffuse", "a.png"}, {"parallax", "b.png"}};
    std::vector<std::string> unknown;
    const auto out = RemapFromJson(json, &unknown);
    REQUIRE(out.size() == 1);
    CHECK(out[0].slot == TextureSlot::Diffuse);
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "parallax");
}

RAVEN_TEST("remap: meta sidecar persists textureRemap across read/write") {
    const auto dir = std::filesystem::temp_directory_path() / ("raven_remap_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const auto model = dir / "model.obj";
    { std::ofstream out(model); out << "o crate"; }
    AssetMeta meta;
    meta.uuid = UUID::Generate();
    meta.type = AssetType::Mesh;
    meta.relativePath = "model.obj";
    meta.importSettings = {{"flipUVs", true}};
    meta.textureRemap = RemapToJson({{TextureSlot::Diffuse, "textures/d.png"}, {TextureSlot::Height, ""}});

    std::string err;
    CHECK(WriteMeta(MetaPathFor(model), meta, &err));
    const auto loaded = ReadMeta(MetaPathFor(model), &err);
    REQUIRE(loaded.has_value());
    CHECK(loaded->uuid == meta.uuid);
    CHECK(loaded->textureRemap == meta.textureRemap);

    // Legacy sidecar without a textureRemap key still parses (empty object).
    const auto legacyPath = dir / "legacy.obj.meta";
    { std::ofstream out(legacyPath); out << "{\"uuid\": \"11111111-1111-1111-1111-111111111111\", \"assetType\": \"mesh\", \"relativePath\": \"legacy.obj\"}"; }
    const auto legacy = ReadMeta(legacyPath, &err);
    REQUIRE(legacy.has_value());
    CHECK(legacy->textureRemap.is_object() && legacy->textureRemap.empty());

    std::filesystem::remove_all(dir, ec);
}
