#include "editor/widgets/ThumbnailGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "stb_image.h"
#include "stb_image_write.h"

namespace raven::editor::thumbnail {

namespace {
constexpr uint32_t kMaxBox = 128;   // §10.2: aspect-preserving fit into a 128px box

// Bilinear fit: scale the source so the longer side lands on `box`, preserving aspect.
std::vector<uint8_t> FitRgba(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                             uint32_t* outW, uint32_t* outH) {
    float scale = 1.0f;
    if (srcW >= srcH && srcW > kMaxBox) scale = static_cast<float>(kMaxBox) / static_cast<float>(srcW);
    else if (srcH > srcW && srcH > kMaxBox) scale = static_cast<float>(kMaxBox) / static_cast<float>(srcH);
    const uint32_t dstW = std::max(1u, static_cast<uint32_t>(std::lround(srcW * scale)));
    const uint32_t dstH = std::max(1u, static_cast<uint32_t>(std::lround(srcH * scale)));
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);
    if (dstW == srcW && dstH == srcH) {
        std::memcpy(dst.data(), src, dst.size());
        *outW = dstW; *outH = dstH;
        return dst;
    }
    for (uint32_t y = 0; y < dstH; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) * (static_cast<float>(srcH) / dstH) - 0.5f,
                                    0.0f, static_cast<float>(srcH - 1));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
        const uint32_t y1 = std::min(y0 + 1, srcH - 1);
        const float wy = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (uint32_t x = 0; x < dstW; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) * (static_cast<float>(srcW) / dstW) - 0.5f,
                                        0.0f, static_cast<float>(srcW - 1));
            const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
            const uint32_t x1 = std::min(x0 + 1, srcW - 1);
            const float wx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            const uint8_t* p00 = src + (static_cast<size_t>(y0) * srcW + x0) * 4;
            const uint8_t* p10 = src + (static_cast<size_t>(y0) * srcW + x1) * 4;
            const uint8_t* p01 = src + (static_cast<size_t>(y1) * srcW + x0) * 4;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * srcW + x1) * 4;
            uint8_t* out = dst.data() + (static_cast<size_t>(y) * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = p00[c] + (p10[c] - p00[c]) * wx;
                const float bottom = p01[c] + (p11[c] - p01[c]) * wx;
                out[c] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * wy, 0.0f, 255.0f) + 0.5f);
            }
        }
    }
    *outW = dstW; *outH = dstH;
    return dst;
}
} // namespace

BakeResult BakeTexture(const std::filesystem::path& source) {
    BakeResult result;
    int w = 0, h = 0, channels = 0;
    stbi_uc* decoded = stbi_load(source.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        result.error = std::string("stb_image decode failed") +
                       (stbi_failure_reason() ? std::string(": ") + stbi_failure_reason() : std::string());
        return result;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(decoded);
        result.error = "texture has no pixels";
        return result;
    }
    uint32_t outW = 0, outH = 0;
    result.rgba = FitRgba(decoded, static_cast<uint32_t>(w), static_cast<uint32_t>(h), &outW, &outH);
    stbi_image_free(decoded);
    result.width = outW;
    result.height = outH;
    result.ok = true;
    return result;
}

bool WritePngAtomic(const std::filesystem::path& path, uint32_t width, uint32_t height,
                    const uint8_t* rgba, std::string* error) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    if (!stbi_write_png(tmp.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                        4, rgba, static_cast<int>(width) * 4)) {
        if (error) *error = "stb_image_write encode failed";
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "thumbnail rename failed: " + ec.message();
        return false;
    }
    return true;
}

std::vector<uint8_t> LoadPng(const std::filesystem::path& path, uint32_t* outWidth,
                             uint32_t* outHeight) {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0, channels = 0;
    stbi_uc* decoded = stbi_load(path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (decoded == nullptr || w <= 0 || h <= 0) {
        if (decoded) stbi_image_free(decoded);
        return pixels;
    }
    pixels.assign(decoded, decoded + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(decoded);
    *outWidth = static_cast<uint32_t>(w);
    *outHeight = static_cast<uint32_t>(h);
    return pixels;
}

} // namespace raven::editor::thumbnail
