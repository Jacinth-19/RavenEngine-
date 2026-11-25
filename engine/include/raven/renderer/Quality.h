// RavenEngine — quality tiers and render-resolution math for low-resource hardware.
//
// The engine's performance philosophy: every expensive feature must have a
// low-resource sibling, and every frame budget must be derivable from the numbers
// below. All functions in this header are pure math (no Vulkan, no globals) so they
// are unit-testable on any platform.
//
// Targets (docs/PERFORMANCE.md has the full derivation):
//   Low    — internal render scale ~0.5, minimal shadows/MSAA, small texture budget
//   Medium — internal render scale ~0.67 (e.g. 1280x720 -> 1080p), 1024 shadows, 2x MSAA
//   High   — native 1.0, 2048 shadow, 4x MSAA — the "AAA look on a mid-range GPU" tier
//   Ultra  — native 1.0 supersampling-class features (SSAO/SSR/TAA), 4096 shadow, 8x MSAA
#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace raven {

enum class QualityTier : uint32_t { Low = 0, Medium = 1, High = 2, Ultra = 3 };

enum class LodBias : uint32_t { Conservative = 0, Default = 1, Aggressive = 2 };

struct QualityPreset {
    QualityTier tier = QualityTier::Medium;
    const char* name = "Medium";

    // --- Resolution path ---------------------------------------------------
    float resolutionScale = 1.0f;   // multiply target extent; <1 = upscaled from a smaller buffer

    // --- Shadows (the single most cache-unfriendly pass in forward renderers) --
    uint32_t shadowMapSize = 1024;  // square depth texture edge (power of two)
    float shadowDistance = 60.0f;   // meters of usable shadow frustum
    uint32_t shadowCascades = 1;    // 1 today (single cascade); reserved for CSM

    // --- MSAA / texture budget ---------------------------------------------
    uint32_t msaaSamples = 2;       // 1 disables MSAA entirely
    uint32_t maxTextureSize = 1024; // longest edge enforced at import time
    uint32_t textureAniso = 2;      // 1 = off

    // --- Lighting budget (forward path) -------------------------------------
    uint32_t maxPointLights = 8;

    // --- LOD policy ----------------------------------------------------------
    float lodBias = 0.0f;           // 0 = engine default; >0 biases toward coarser LOD sooner
    float lodMaxDistance = 250.0f;  // beyond this, invisible-culled entirely

    // --- Feature gates (consumed by forward/deferred loops & tools) ---------
    bool ssao = false;
    bool bloom = false;
    bool ssr = false;
    bool taa = false;
    bool motionBlur = false;

    // --- Simulation budgets ---------------------------------------------------
    float physicsTickRateHz = 60.0f;  // fixed timestep; render may run faster or slower
    uint32_t physicsMaxSubsteps = 4;  // clamp for spiral-of-death protection
    float physicsSleepDistance = 120.0f;  // bodies beyond this are frozen (not integrated)
    uint32_t audioMaxVoices = 32;
    uint32_t maxParticles = 2048;
};

// Preset table. Values only ever *increase* in cost as tier goes Low -> Ultra.
constexpr QualityPreset kQualityPresets[4] = {
    {   // Low — "iGPU 1080p60 @ reduced internal res"
        QualityTier::Low, "Low",
        /*resolutionScale*/ 0.5f,
        /*shadowMapSize*/ 512, /*shadowDistance*/ 30.0f, /*shadowCascades*/ 1,
        /*msaaSamples*/ 1, /*maxTextureSize*/ 512, /*textureAniso*/ 1,
        /*maxPointLights*/ 2,
        /*lodBias*/ 1.0f, /*lodMaxDistance*/ 120.0f,
        /*ssao*/ false, /*bloom*/ false, /*ssr*/ false, /*taa*/ false, /*motionBlur*/ false,
        /*physicsTickRateHz*/ 30.0f, /*physicsMaxSubsteps*/ 2, /*physicsSleepDistance*/ 60.0f,
        /*audioMaxVoices*/ 16, /*maxParticles*/ 512,
    },
    {   // Medium — "integrated graphics / old discrete 1080p60"
        QualityTier::Medium, "Medium",
        /*resolutionScale*/ 0.6666667f,
        /*shadowMapSize*/ 1024, /*shadowDistance*/ 60.0f, /*shadowCascades*/ 1,
        /*msaaSamples*/ 2, /*maxTextureSize*/ 1024, /*textureAniso*/ 2,
        /*maxPointLights*/ 8,
        /*lodBias*/ 0.25f, /*lodMaxDistance*/ 180.0f,
        /*ssao*/ false, /*bloom*/ true, /*ssr*/ false, /*taa*/ false, /*motionBlur*/ false,
        /*physicsTickRateHz*/ 60.0f, /*physicsMaxSubsteps*/ 4, /*physicsSleepDistance*/ 100.0f,
        /*audioMaxVoices*/ 32, /*maxParticles*/ 1024,
    },
    {   // High — "mid-range discrete GPU: native 1440p60 / 4K30"
        QualityTier::High, "High",
        /*resolutionScale*/ 1.0f,
        /*shadowMapSize*/ 2048, /*shadowDistance*/ 90.0f, /*shadowCascades*/ 1,
        /*msaaSamples*/ 4, /*maxTextureSize*/ 2048, /*textureAniso*/ 4,
        /*maxPointLights*/ 16,
        /*lodBias*/ 0.0f, /*lodMaxDistance*/ 250.0f,
        /*ssao*/ true, /*bloom*/ true, /*ssr*/ false, /*taa*/ true, /*motionBlur*/ false,
        /*physicsTickRateHz*/ 60.0f, /*physicsMaxSubsteps*/ 4, /*physicsSleepDistance*/ 150.0f,
        /*audioMaxVoices*/ 48, /*maxParticles*/ 4096,
    },
    {   // Ultra — "enthusiast GPU: native 1440p120 / 4K60"
        QualityTier::Ultra, "Ultra",
        /*resolutionScale*/ 1.0f,
        /*shadowMapSize*/ 4096, /*shadowDistance*/ 120.0f, /*shadowCascades*/ 2,
        /*msaaSamples*/ 8, /*maxTextureSize*/ 4096, /*textureAniso*/ 16,
        /*maxPointLights*/ 32,
        /*lodBias*/ -0.5f, /*lodMaxDistance*/ 400.0f,
        /*ssao*/ true, /*bloom*/ true, /*ssr*/ true, /*taa*/ true, /*motionBlur*/ true,
        /*physicsTickRateHz*/ 120.0f, /*physicsMaxSubsteps*/ 8, /*physicsSleepDistance*/ 250.0f,
        /*audioMaxVoices*/ 64, /*maxParticles*/ 8192,
    },
};

constexpr const QualityPreset& QualityPresetFor(QualityTier tier) {
    const uint32_t index = static_cast<uint32_t>(tier);
    const uint32_t clamped = index < 4 ? index : static_cast<uint32_t>(QualityTier::Medium);
    return kQualityPresets[clamped];
}

// --- Resolution math -------------------------------------------------------------------

constexpr uint64_t PixelsForResolution(uint32_t width, uint32_t height) {
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
}

constexpr float SanitizeResolutionScale(float scale) {
    // Below 0.5 the inputs become aliased beyond what a typical resolve can hide;
    // above 1.0 is supersampling, only meaningful on Ultra.
    return scale < 0.5f ? 0.5f : (scale > 1.0f ? 1.0f : scale);
}

// Internal (render-target) extent for one axis after resolution scaling.
constexpr uint32_t InternalExtent(uint32_t targetExtent, float resolutionScale) {
    const float clamped = SanitizeResolutionScale(resolutionScale);
    const uint32_t v = static_cast<uint32_t>(static_cast<float>(targetExtent) * clamped);
    return v < 1 ? 1 : v;
}

constexpr uint64_t InternalPixelCount(uint32_t targetWidth, uint32_t targetHeight, float resolutionScale) {
    return PixelsForResolution(InternalExtent(targetWidth, resolutionScale),
                               InternalExtent(targetHeight, resolutionScale));
}

// --- LOD math --------------------------------------------------------------------------
//
// Projected screen height of a bounding sphere:
//     h_px = (2 * atan(r / d) / verticalFovRadians) * viewportHeightPx
// where r is the bounding sphere radius, d the distance from the camera to the
// sphere centre, and verticalFovRadians the camera's vertical FOV.
constexpr float VerticalFovRadians(float fovDegrees) {
    constexpr float kDegToRad = 0.017453292519943295f;
    return fovDegrees * kDegToRad;
}

constexpr float ScreenSpaceHeight(float boundingSphereRadius, float distance,
                                  float fovDegrees, uint32_t viewportHeightPx) {
    if (distance <= 1e-4f) distance = 1e-4f;
    if (boundingSphereRadius <= 0.0f) return 0.0f;
    const float subtended = 2.0f * std::atan(boundingSphereRadius / distance);
    const float fovRad = VerticalFovRadians(fovDegrees);
    if (fovRad <= 1e-6f) return 0.0f;
    return (subtended / fovRad) * static_cast<float>(viewportHeightPx);
}

struct LodConfig {
    float toLod1 = 128.0f;   // px — below this, switch from LOD0 to LOD1
    float toLod2 = 64.0f;    // px
    float toLod3 = 24.0f;    // px
    float lodBias = 0.0f;    // multiplicative on the thresholds; >0 => coarser LOD sooner
};

// Returns the selected LOD index (0 = highest detail). maxLod caps the deepest level.
// Whole-object culling is the caller's decision (frustum / max lodMaxDistance); this
// function only picks which LOD to draw.
constexpr int SelectLod(float distance, float boundingSphereRadius, const LodConfig& config,
                        uint32_t viewportHeightPx, float fovDegrees, int maxLod = 3) {
    if (maxLod < 0) maxLod = 0;
    const float bias = config.lodBias < -0.9f ? -0.9f : config.lodBias;  // avoid negative thresholds
    const float biasFactor = 1.0f + bias;
    const float to1 = config.toLod1 * biasFactor;
    const float to2 = config.toLod2 * biasFactor;
    const float to3 = config.toLod3 * biasFactor;
    const float h = ScreenSpaceHeight(boundingSphereRadius, distance, fovDegrees, viewportHeightPx);
    if (maxLod >= 1 && h < to1) {
        if (maxLod >= 2 && h < to2) {
            if (maxLod >= 3 && h < to3) return 3;
            return maxLod >= 2 ? 2 : maxLod;
        }
        return maxLod >= 1 ? 1 : maxLod;
    }
    return 0;
}

// Convenience: budget-side math. Bytes of bandwidth per frame for a BGRA8 + D24S8 surface
// pair at a given pixel count (uncompressed, single pass, no overdraw).
constexpr uint64_t ForwardBaselineBytesPerFrame(uint64_t pixels, float overdraw = 1.0f) {
    // color write (4B) + depth write (4B) + depth read (4B) = 12 B/px baseline
    constexpr uint64_t kBytesPerPixel = 12;
    return static_cast<uint64_t>(static_cast<float>(pixels * kBytesPerPixel) * overdraw);
}

} // namespace raven
