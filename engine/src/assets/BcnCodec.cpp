#include "raven/assets/BcnCodec.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>

#include <bc7enc.h>
#include <bc7decomp.h>
#include <rgbcx.h>

#include "raven/core/Logger.h"

namespace raven {

const char* BcnFormatToString(BcnFormat format) {
    switch (format) {
        case BcnFormat::BC1: return "bc1";
        case BcnFormat::BC5: return "bc5";
        case BcnFormat::BC7: return "bc7";
    }
    return "bc7";
}

std::optional<BcnFormat> BcnFormatFromString(std::string_view s) {
    if (s.size() != 3) return std::nullopt;
    std::string lower;
    lower.reserve(3);
    for (const char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "bc1") return BcnFormat::BC1;
    if (lower == "bc5") return BcnFormat::BC5;
    if (lower == "bc7") return BcnFormat::BC7;
    return std::nullopt;
}

std::optional<BcnFormat> BcnFormatFromSetting(const nlohmann::json& importSettings) {
    if (!importSettings.is_object() || !importSettings.contains("compress")) return std::nullopt;
    const auto& value = importSettings["compress"];
    if (value.is_boolean()) return value.get<bool>() ? BcnFormat::BC7 : std::optional<BcnFormat>{};
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s.empty()) return std::nullopt;
        return BcnFormatFromString(s);
    }
    return std::nullopt;
}

size_t BcnBytesPerBlock(BcnFormat format) { return format == BcnFormat::BC1 ? 8 : 16; }
size_t BcnBlockCount(uint32_t width, uint32_t height) {
    const size_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    return bw * bh;
}

namespace {

void EnsureRgbcxInitialized() {
    static std::once_flag s_Once;
    std::call_once(s_Once, [] { rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); });
}

void EnsureBc7Initialized() {
    static std::once_flag s_Once;
    std::call_once(s_Once, [] { bc7enc_compress_block_init(); });
}

// Copies an RGBA8 image into an edge-replicated buffer whose dimensions are
// multiples of 4, so block encoders always see full 4x4 blocks.
std::vector<uint8_t> PadToBlocks(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t paddedWidth) {
    const uint32_t paddedHeight = (height + 3) / 4 * 4;
    std::vector<uint8_t> padded(static_cast<size_t>(paddedWidth) * paddedHeight * 4, 0);
    for (uint32_t y = 0; y < paddedHeight; ++y) {
        const uint32_t srcY = std::min(y, height - 1);
        for (uint32_t x = 0; x < paddedWidth; ++x) {
            const uint32_t srcX = std::min(x, width - 1);
            const uint8_t* src = rgba + (static_cast<size_t>(srcY) * width + srcX) * 4;
            uint8_t* dst = padded.data() + (static_cast<size_t>(y) * paddedWidth + x) * 4;
            std::memcpy(dst, src, 4);
        }
    }
    return padded;
}

} // namespace

std::vector<uint8_t> EncodeBcnBlocks(BcnFormat format, const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return {};
    if (format == BcnFormat::BC1 || format == BcnFormat::BC5) EnsureRgbcxInitialized();
    if (format == BcnFormat::BC7) EnsureBc7Initialized();

    const uint32_t paddedWidth = (width + 3) / 4 * 4;
    const uint32_t paddedHeight = (height + 3) / 4 * 4;
    const std::vector<uint8_t> padded = PadToBlocks(rgba, width, height, paddedWidth);

    const size_t blockCount = static_cast<size_t>(paddedWidth / 4) * (paddedHeight / 4);
    const size_t blockBytes = BcnBytesPerBlock(format);
    std::vector<uint8_t> blocks(blockCount * blockBytes);

    // Prepared BC7 params: perceptual YCbCr weights (color data), fast preset.
    bc7enc_compress_block_params bc7Params{};
    if (format == BcnFormat::BC7) {
        bc7enc_compress_block_params_init(&bc7Params);   // perceptual weights, uber_level 0
    }

    for (uint32_t by = 0; by < paddedHeight; by += 4) {
        for (uint32_t bx = 0; bx < paddedWidth; bx += 4) {
            const size_t blockIndex = (static_cast<size_t>(by / 4) * (paddedWidth / 4)) + (bx / 4);
            uint8_t* block = blocks.data() + blockIndex * blockBytes;

            // Gather the 16 pixels of this block into contiguous RGBA order.
            uint8_t pixels[64];
            for (uint32_t dy = 0; dy < 4; ++dy) {
                const uint8_t* row = padded.data() + (static_cast<size_t>(by + dy) * paddedWidth + bx) * 4;
                std::memcpy(pixels + dy * 16, row, 16);
            }

            switch (format) {
                case BcnFormat::BC1:
                    rgbcx::encode_bc1(/*level*/ 10, block, pixels, /*allow_3color*/ true, /*use_transparent_black*/ false);
                    break;
                case BcnFormat::BC5:
                    rgbcx::encode_bc5_hq(block, pixels, /*chan0*/ 0, /*chan1*/ 1, /*stride*/ 4);
                    break;
                case BcnFormat::BC7:
                    bc7enc_compress_block(block, pixels, &bc7Params);
                    break;
            }
        }
    }
    return blocks;
}

std::vector<uint8_t> DecodeBcnBlocks(BcnFormat format, const uint8_t* blocks, uint32_t width, uint32_t height) {
    if (!blocks || width == 0 || height == 0) return {};
    if (format == BcnFormat::BC1 || format == BcnFormat::BC5) EnsureRgbcxInitialized();

    const uint32_t paddedWidth = (width + 3) / 4 * 4;
    const uint32_t paddedHeight = (height + 3) / 4 * 4;
    const size_t blockBytes = BcnBytesPerBlock(format);
    std::vector<uint8_t> padded(static_cast<size_t>(paddedWidth) * paddedHeight * 4, 0);

    // BC5 leaves B/A alone: pre-fill the padded image with opaque black.
    if (format == BcnFormat::BC5) {
        for (size_t i = 0; i < padded.size(); i += 4) { padded[i + 2] = 0; padded[i + 3] = 255; }
    }

    for (uint32_t by = 0; by < paddedHeight; by += 4) {
        for (uint32_t bx = 0; bx < paddedWidth; bx += 4) {
            const size_t blockIndex = (static_cast<size_t>(by / 4) * (paddedWidth / 4)) + (bx / 4);
            const uint8_t* block = blocks + blockIndex * blockBytes;
            uint8_t pixels[64];
            switch (format) {
                case BcnFormat::BC1:
                    rgbcx::unpack_bc1(block, pixels, /*set_alpha*/ true);
                    break;
                case BcnFormat::BC5:
                    rgbcx::unpack_bc5(block, pixels, /*chan0*/ 0, /*chan1*/ 1, /*stride*/ 4);
                    break;
                case BcnFormat::BC7:
                    bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(pixels));
                    break;
            }
            for (uint32_t dy = 0; dy < 4; ++dy) {
                uint8_t* row = padded.data() + (static_cast<size_t>(by + dy) * paddedWidth + bx) * 4;
                if (format == BcnFormat::BC5) {
                    // Unpack writes R,G at stride 4; splice them into the padded image.
                    for (uint32_t dx = 0; dx < 4; ++dx) {
                        row[dx * 4 + 0] = pixels[dy * 16 + dx * 4 + 0];
                        row[dx * 4 + 1] = pixels[dy * 16 + dx * 4 + 1];
                    }
                } else {
                    std::memcpy(row, pixels + dy * 16, 16);
                }
            }
        }
    }

    // Crop padding.
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * width * 4,
                    padded.data() + static_cast<size_t>(y) * paddedWidth * 4,
                    static_cast<size_t>(width) * 4);
    }
    return out;
}

namespace {
void WritePod(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
void WritePod64(std::ofstream& out, uint64_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
} // namespace

bool WriteBcnFile(const std::filesystem::path& path, BcnFormat format,
                  const std::vector<std::pair<uint32_t, uint32_t>>& mipDimensions,
                  const std::vector<std::vector<uint8_t>>& mipBlocks, std::string* error) {
    if (mipDimensions.size() != mipBlocks.size()) {
        if (error) *error = "mip dimension/block count mismatch";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out.write("RBC7", 4);
    WritePod(out, /*version*/ 1);
    WritePod(out, static_cast<uint32_t>(format));
    WritePod(out, static_cast<uint32_t>(mipDimensions.size()));
    for (size_t i = 0; i < mipDimensions.size(); ++i) {
        WritePod(out, mipDimensions[i].first);
        WritePod(out, mipDimensions[i].second);
        WritePod64(out, static_cast<uint64_t>(mipBlocks[i].size()));
    }
    for (const auto& blocks : mipBlocks) out.write(reinterpret_cast<const char*>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
    if (!out) {
        if (error) *error = "failed writing " + path.string();
        return false;
    }
    return true;
}

} // namespace raven
