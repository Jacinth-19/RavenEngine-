// Import-time BCn compression (§16 Phase 3) — codec correctness + end-to-end
// .bc7 artifact written by TextureImporter when the .meta asks for compression.
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <stb_image_write.h>
#include <nlohmann/json.hpp>

#include "raven/assets/Asset.h"
#include "raven/assets/BcnCodec.h"
#include "raven/assets/Importers.h"
#include "raven/core/Logger.h"
#include "raven/core/UUID.h"

namespace fs = std::filesystem;
using namespace raven;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        static uint64_t counter = 0;
        root = fs::temp_directory_path() / ("raven_bcn_" + std::to_string(++counter) + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(root, ec);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(root, ec); }
};

// Smooth 2D color field — BCn loves smooth data, so error bounds are tight.
std::vector<uint8_t> MakeGradientImage(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>((x * 255) / std::max(1u, w - 1));
            p[1] = static_cast<uint8_t>((y * 255) / std::max(1u, h - 1));
            p[2] = static_cast<uint8_t>(((x + y) * 255) / std::max(1u, (w + h - 2)));
            p[3] = 255;
        }
    }
    return px;
}

// Synthetic normal map: smooth U/V variation in R/G, B=255 (Z up).
std::vector<uint8_t> MakeNormalImage(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>(128 + ((x * 96) / w) - 48);
            p[1] = static_cast<uint8_t>(128 + ((y * 96) / h) - 48);
            p[2] = 255;
            p[3] = 255;
        }
    }
    return px;
}

struct ErrorStats { double mean = 0.0; int maxAbs = 0; };

ErrorStats ChannelStats(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channelOffset) {
    double sum = 0.0;
    int maxAbs = 0;
    const size_t px = b.size() / 4;
    for (size_t i = 0; i < px; ++i) {
        const int d = std::abs(static_cast<int>(a[i * 4 + channelOffset]) - static_cast<int>(b[i * 4 + channelOffset]));
        sum += d;
        if (d > maxAbs) maxAbs = d;
    }
    return {sum / static_cast<double>(px), maxAbs};
}

constexpr uint32_t kSmall = 64;

} // namespace

RAVEN_TEST("bcn: compress setting parse accepts legacy bool and format strings") {
    CHECK(!BcnFormatFromSetting(nlohmann::json::object()).has_value());
    CHECK(!BcnFormatFromSetting(nlohmann::json{{"compress", false}}).has_value());
    CHECK(BcnFormatFromSetting(nlohmann::json{{"compress", true}}) == BcnFormat::BC7);    // legacy bool
    CHECK(BcnFormatFromSetting(nlohmann::json{{"compress", ""}}) == std::nullopt);
    CHECK(BcnFormatFromSetting(nlohmann::json{{"compress", "bc1"}}) == BcnFormat::BC1);
    CHECK(BcnFormatFromSetting(nlohmann::json{{"compress", "BC5"}}) == BcnFormat::BC5);
    CHECK(BcnFormatFromSetting(nlohmann::json{{"compress", "bC7"}}) == BcnFormat::BC7);
    CHECK(!BcnFormatFromSetting(nlohmann::json{{"compress", "bc6"}}).has_value());
    CHECK(!BcnFormatFromSetting(nlohmann::json{{"compress", 42}}).has_value());
    CHECK(BcnFormatToString(BcnFormat::BC1) == std::string("bc1"));
    CHECK(BcnFormatToString(BcnFormat::BC5) == std::string("bc5"));
    CHECK(BcnFormatToString(BcnFormat::BC7) == std::string("bc7"));
    CHECK(BcnFormatFromString("BC1") == BcnFormat::BC1);
    CHECK(!BcnFormatFromString("bc11").has_value());
    CHECK(BcnBytesPerBlock(BcnFormat::BC1) == 8u);
    CHECK(BcnBytesPerBlock(BcnFormat::BC5) == 16u);
    CHECK(BcnBytesPerBlock(BcnFormat::BC7) == 16u);
}

RAVEN_TEST("bcn: BC7 roundtrip stays within tolerance on smooth color") {
    const auto rgba = MakeGradientImage(kSmall, kSmall);
    const std::vector<uint8_t> blocks = EncodeBcnBlocks(BcnFormat::BC7, rgba.data(), kSmall, kSmall);
    CHECK_EQ(blocks.size(), size_t((kSmall / 4) * (kSmall / 4) * 16));

    const std::vector<uint8_t> back = DecodeBcnBlocks(BcnFormat::BC7, blocks.data(), kSmall, kSmall);
    CHECK_EQ(back.size(), rgba.size());

    const ErrorStats rgb = ChannelStats(back, rgba, 0);
    const ErrorStats g = ChannelStats(back, rgba, 1);
    const ErrorStats b = ChannelStats(back, rgba, 2);
    const ErrorStats a = ChannelStats(back, rgba, 3);
    RAVEN_LOG_INFO("bcn BC7 mean err r=%.2f g=%.2f b=%.2f a=%.2f max=%d", rgb.mean, g.mean, b.mean, a.mean,
                   std::max({rgb.maxAbs, g.maxAbs, b.maxAbs, a.maxAbs}));
    CHECK(rgb.mean < 8.0 && g.mean < 8.0 && b.mean < 8.0);          // smooth data: near-lossless
    CHECK(a.mean < 1.0);                                            // alpha is constant 255
    CHECK(std::max({rgb.maxAbs, g.maxAbs, b.maxAbs}) < 64);
}

RAVEN_TEST("bcn: BC1 roundtrip stays within tolerance and is 8 bytes per block") {
    const auto rgba = MakeGradientImage(kSmall, kSmall);
    const std::vector<uint8_t> blocks = EncodeBcnBlocks(BcnFormat::BC1, rgba.data(), kSmall, kSmall);
    CHECK_EQ(blocks.size(), size_t((kSmall / 4) * (kSmall / 4) * 8));

    const std::vector<uint8_t> back = DecodeBcnBlocks(BcnFormat::BC1, blocks.data(), kSmall, kSmall);
    CHECK_EQ(back.size(), rgba.size());

    const ErrorStats r = ChannelStats(back, rgba, 0);
    const ErrorStats g = ChannelStats(back, rgba, 1);
    const ErrorStats b = ChannelStats(back, rgba, 2);
    RAVEN_LOG_INFO("bcn BC1 mean err r=%.2f g=%.2f b=%.2f max=%d", r.mean, g.mean, b.mean,
                   std::max({r.maxAbs, g.maxAbs, b.maxAbs}));
    CHECK(r.mean < 16.0 && g.mean < 16.0 && b.mean < 16.0);
    CHECK(std::max({r.maxAbs, g.maxAbs, b.maxAbs}) < 96);
}

RAVEN_TEST("bcn: BC5 encodes two channels with B=0 and A=255 on decode") {
    const auto normal = MakeNormalImage(kSmall, kSmall);
    const std::vector<uint8_t> blocks = EncodeBcnBlocks(BcnFormat::BC5, normal.data(), kSmall, kSmall);
    CHECK_EQ(blocks.size(), size_t((kSmall / 4) * (kSmall / 4) * 16));

    const std::vector<uint8_t> back = DecodeBcnBlocks(BcnFormat::BC5, blocks.data(), kSmall, kSmall);
    CHECK_EQ(back.size(), normal.size());

    const ErrorStats r = ChannelStats(back, normal, 0);
    const ErrorStats g = ChannelStats(back, normal, 1);
    RAVEN_LOG_INFO("bcn BC5 mean err r=%.2f g=%.2f max=%d", r.mean, g.mean, std::max(r.maxAbs, g.maxAbs));
    CHECK(r.mean < 8.0 && g.mean < 8.0);
    CHECK(std::max(r.maxAbs, g.maxAbs) < 48);
    for (size_t i = 0; i < back.size(); i += 4) {
        CHECK(back[i + 2] == 0);     // B untouched by BC5
        CHECK(back[i + 3] == 255);   // A forced opaque
    }
}

RAVEN_TEST("bcn: non-multiple-of-4 dimensions are edge-padded and decode back cropped") {
    const uint32_t w = 63, h = 17;
    const auto rgba = MakeGradientImage(w, h);
    const std::vector<uint8_t> blocks = EncodeBcnBlocks(BcnFormat::BC7, rgba.data(), w, h);
    const size_t blocksX = (w + 3) / 4, blocksY = (h + 3) / 4;
    CHECK_EQ(blocks.size(), size_t(blocksX * blocksY * 16));

    const std::vector<uint8_t> back = DecodeBcnBlocks(BcnFormat::BC7, blocks.data(), w, h);
    CHECK_EQ(back.size(), rgba.size());   // cropped to the source dimensions
}

RAVEN_TEST("bcn: TextureImporter writes a parseable .bc7 artifact when compress is set") {
    TempDir dir;
    const fs::path pngPath = dir.root / "normal.png";
    const auto normal = MakeNormalImage(128, 128);
    REQUIRE(stbi_write_png(pngPath.string().c_str(), 128, 128, 4, normal.data(), 128 * 4) != 0);

    for (const char* setting : {"bc5", "true"}) {   // explicit + legacy bool form
        nlohmann::json settings{{"srgb", false}, {"generateMipmaps", true}};
        if (std::string(setting) == "true") settings["compress"] = true;
        else settings["compress"] = setting;
        settings["bc7CacheDir"] = (dir.root / "cache").string();

        const UUID uuid = UUID::Generate();
        const ImportResult result = TextureImporter::Import(pngPath, settings, uuid);
        CHECK(result.ok);
        if (!result.ok) continue;

        const fs::path artifact = dir.root / "cache" / (uuid.ToString() + ".bc7");
        REQUIRE(fs::exists(artifact));
        const std::streamsize size = fs::file_size(artifact);
        std::ifstream in(artifact, std::ios::binary);
        char magic[4] = {};
        uint32_t version = 0, format = 0, mipCount = 0;
        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&format), 4);
        in.read(reinterpret_cast<char*>(&mipCount), 4);
        CHECK(std::memcmp(magic, "RBC7", 4) == 0);
        CHECK_EQ(version, 1u);
        CHECK_EQ(format, std::string(setting) == "bc5" ? 2u : 3u);
        CHECK_EQ(mipCount, 8u);   // 128 → 1

        uint64_t total = 16;   // magic(4) + version(4) + format(4) + mipCount(4)
        uint32_t mw = 128, mh = 128;
        for (uint32_t i = 0; i < mipCount; ++i) {
            uint32_t w = 0, h = 0;
            uint64_t bytes = 0;
            in.read(reinterpret_cast<char*>(&w), 4);
            in.read(reinterpret_cast<char*>(&h), 4);
            in.read(reinterpret_cast<char*>(&bytes), 8);
            CHECK_EQ(w, mw);
            CHECK_EQ(h, mh);
            const size_t expectBlocks = ((static_cast<size_t>(mw) + 3) / 4) * ((mh + 3) / 4);
            const size_t expectBytes = expectBlocks * 16;   // BC5 and BC7 are both 16-byte blocks
            CHECK_EQ(bytes, static_cast<uint64_t>(expectBytes));
            total += 16 + bytes;
            mw = std::max(1u, mw / 2);
            mh = std::max(1u, mh / 2);
        }
        CHECK_EQ(static_cast<uint64_t>(size), total);
        CHECK(static_cast<bool>(in));
    }
}

RAVEN_TEST("bcn: TextureImporter without bc7CacheDir skips the artifact but still imports") {
    TempDir dir;
    const fs::path pngPath = dir.root / "plain.png";
    const auto rgba = MakeGradientImage(32, 32);
    REQUIRE(stbi_write_png(pngPath.string().c_str(), 32, 32, 4, rgba.data(), 32 * 4) != 0);

    nlohmann::json settings{{"compress", "bc7"}};   // no bc7CacheDir — warn, not fail
    const ImportResult result = TextureImporter::Import(pngPath, settings, UUID::Generate());
    CHECK(result.ok);
    CHECK(!fs::exists(dir.root / "cache") || fs::is_empty(dir.root / "cache"));
}
