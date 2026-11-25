// Quality tiers, render-resolution math and LOD selection — the low-resource contract.
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "raven/renderer/Quality.h"
#include "raven/renderer/Renderer.h"

using namespace raven;

RAVEN_TEST("quality: pixel counts for the two target resolutions") {
    CHECK(PixelsForResolution(1920, 1080) == 2073600ULL);
    CHECK(PixelsForResolution(2560, 1440) == 3686400ULL);
    CHECK(std::fabs(static_cast<float>(PixelsForResolution(2560, 1440)) /
                    static_cast<float>(PixelsForResolution(1920, 1080)) - 16.0f / 9.0f) < 1e-3f);
}

RAVEN_TEST("quality: internal resolution honors the render scale") {
    CHECK(InternalExtent(1920, 1.0f) == 1920);
    CHECK(InternalExtent(2560, 0.5f) == 1280);
    CHECK(InternalExtent(1440, 0.5f) == 720);
    // Medium tier ~0.67: 1080p renders ~1280x720-class internally, 1440p ~1707x960
    CHECK(InternalExtent(1920, 0.6666667f) == 1280);
    CHECK(InternalExtent(2560, 0.6666667f) == 1706);   // floor(2560 * 2/3)
    CHECK(InternalPixelCount(1920, 1080, 0.5f) == 518400ULL);  // 960x540
    CHECK(InternalPixelCount(2560, 1440, 0.6666667f) ==
          static_cast<uint64_t>(InternalExtent(2560, 0.6666667f)) * InternalExtent(1440, 0.6666667f));
    // Scale is clamped to a sane band
    CHECK(SanitizeResolutionScale(0.1f) == 0.5f);
    CHECK(SanitizeResolutionScale(1.5f) == 1.0f);
}

RAVEN_TEST("quality: preset table is monotonic in cost (Low -> Ultra)") {
    for (int i = 1; i < 4; ++i) {
        const QualityPreset& lo = kQualityPresets[i - 1];
        const QualityPreset& hi = kQualityPresets[i];
        CHECK(hi.shadowMapSize >= lo.shadowMapSize);
        CHECK(hi.maxTextureSize >= lo.maxTextureSize);
        CHECK(hi.msaaSamples >= lo.msaaSamples);
        CHECK(hi.maxPointLights >= lo.maxPointLights);
        CHECK(hi.maxParticles >= lo.maxParticles);
        CHECK(hi.resolutionScale >= lo.resolutionScale);
    }
    // The two low-end tiers use upscaled internal resolution; High/Ultra render native.
    CHECK(kQualityPresets[0].resolutionScale < 1.0f);
    CHECK(kQualityPresets[1].resolutionScale < 1.0f);
    CHECK(kQualityPresets[2].resolutionScale == 1.0f);
    CHECK(kQualityPresets[3].resolutionScale == 1.0f);
    // Low tier disables the expensive passes entirely
    CHECK(!kQualityPresets[0].ssao);
    CHECK(!kQualityPresets[0].ssr);
    CHECK(!kQualityPresets[0].taa);
    // Ultra enables them
    CHECK(kQualityPresets[3].ssao);
    CHECK(kQualityPresets[3].ssr);
    CHECK(kQualityPresets[3].taa);
}

RAVEN_TEST("quality: QualityPresetFor returns the requested tier and clamps garbage") {
    CHECK(QualityPresetFor(QualityTier::Low).shadowMapSize == 512);
    CHECK(QualityPresetFor(QualityTier::Medium).shadowMapSize == 1024);
    CHECK(QualityPresetFor(QualityTier::High).shadowMapSize == 2048);
    CHECK(QualityPresetFor(QualityTier::Ultra).shadowMapSize == 4096);
    CHECK(QualityPresetFor(static_cast<QualityTier>(77)).name[0] == 'M');  // falls back to Medium
}

RAVEN_TEST("quality: ScreenSpaceHeight matches the closed-form projection") {
    // h = (2*atan(r/d) / fovRad) * h_px. For r=0.5, d=10, fov=60, h=1080 the expected
    // value is ~34.9 px (derivable by hand — see docs/PERFORMANCE.md).
    const float h = ScreenSpaceHeight(0.5f, 10.0f, 60.0f, 1080);
    const float expected = (2.0f * std::atan(0.05f) / (60.0f * 0.017453292519943295f)) * 1080.0f;
    CHECK(std::fabs(h - expected) < 1e-3f);
    // Halving the distance more than doubles the projected height (atan curve, so slight sub-linearity)
    CHECK(ScreenSpaceHeight(0.5f, 5.0f, 60.0f, 1080) > 2.0f * h * 0.99f);
    // Zero radius / zero FOV are safe
    CHECK(ScreenSpaceHeight(0.0f, 10.0f, 60.0f, 1080) == 0.0f);
    CHECK(ScreenSpaceHeight(0.5f, 10.0f, 0.0f, 1080) == 0.0f);
}

RAVEN_TEST("quality: SelectLod picks finer LOD for nearer objects") {
    LodConfig cfg;   // toLod1=128, toLod2=64, toLod3=24 px
    const float radius = 1.0f;    // 1 m bounding sphere
    const float fov = 60.0f;
    const uint32_t vp = 1080;
    // Near: projects large -> LOD0
    CHECK(SelectLod(2.0f, radius, cfg, vp, fov, 3) == 0);
    // ~10 m: height ~ (2*atan(0.1)/1.0472)*1080 ≈ 206 px -> LOD0
    CHECK(SelectLod(10.0f, radius, cfg, vp, fov, 3) == 0);
    // ~20 m: height ≈ 103 px -> below toLod1 -> LOD1
    CHECK(SelectLod(20.0f, radius, cfg, vp, fov, 3) == 1);
    // ~40 m: height ≈ 52 px -> below toLod2 -> LOD2
    CHECK(SelectLod(40.0f, radius, cfg, vp, fov, 3) == 2);
    // ~90 m: height ≈ 23 px -> below toLod3 -> LOD3
    CHECK(SelectLod(90.0f, radius, cfg, vp, fov, 3) == 3);
    // maxLod caps the depth
    CHECK(SelectLod(90.0f, radius, cfg, vp, fov, 1) == 1);
    // lodBias>0 biases toward coarser selection sooner
    LodConfig eager; eager.lodBias = 1.0f;   // doubles the thresholds
    CHECK(SelectLod(10.0f, radius, eager, vp, fov, 3) == 1);
    // lodBias<0 biases toward finer selection
    LodConfig greedy; greedy.lodBias = -0.5f;  // halves the thresholds
    CHECK(SelectLod(40.0f, radius, greedy, vp, fov, 3) == 1);
}

RAVEN_TEST("quality: RendererConfig defaults keep the safe Medium tier") {
    RendererConfig cfg;
    CHECK(cfg.qualityTier == QualityTier::Medium);
    CHECK(cfg.resolutionScale == 1.0f);
}
