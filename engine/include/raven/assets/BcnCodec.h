// RavenEngine — import-time BCn texture compression (BC7 / BC5 / BC1).
//
// CPU-only codec used by the asset pipeline when a texture's .meta sets
// "compress". GPU upload (transfer-queue copy of the blocks into a
// VK_FORMAT_BC* image) lands with the renderer's compressed-format path; until
// then the encoder output is persisted at <LibraryRoot>/bc7/<uuid>.bc7 and
// consumed by nothing at runtime.
//
// BC7  — 16-byte RGBA blocks (color)      → VK_FORMAT_BC7_SRGB_BLOCK / _UNORM
// BC5  — 16-byte two-channel blocks        → VK_FORMAT_BC5_UNORM (normal maps)
// BC1  — 8-byte RGB blocks, no alpha       → VK_FORMAT_BC1_RGB_SRGB_BLOCK
//
// Blocks are computed over 4x4 tiles with edge replication for dimensions that
// are not multiples of 4. This header is pure data — no Vulkan types.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace raven {

enum class BcnFormat : uint8_t { BC1 = 1, BC5 = 2, BC7 = 3 };

const char* BcnFormatToString(BcnFormat format);                 // "bc1" | "bc5" | "bc7"
std::optional<BcnFormat> BcnFormatFromString(std::string_view s); // case-insensitive

// Parses the .meta `compress` setting. Missing/false/"" → nullopt (off).
// true → BC7 (legacy boolean form written by earlier editors).
// "bc1" | "bc5" | "bc7" → that format (case-insensitive). Anything else → nullopt.
std::optional<BcnFormat> BcnFormatFromSetting(const nlohmann::json& importSettings);

size_t BcnBytesPerBlock(BcnFormat format);   // 8 (BC1) or 16 (BC5/BC7)
size_t BcnBlockCount(uint32_t width, uint32_t height);

// Encodes one mip level of an RGBA8 image to tightly packed blocks.
// Returns empty vector on failure (never throws; dimensions must be > 0).
std::vector<uint8_t> EncodeBcnBlocks(BcnFormat format, const uint8_t* rgba, uint32_t width, uint32_t height);

// Decodes a tightly packed block array back to RGBA8 (edge-padded size cropped
// to width×height). BC5 writes R,G from the two channels and B=0,A=255 per
// pixel. Returns empty vector on malformed input. Used by tests and tooling.
std::vector<uint8_t> DecodeBcnBlocks(BcnFormat format, const uint8_t* blocks, uint32_t width, uint32_t height);

// .bc7 container: "RBC7" | u32 version(1) | u32 format | u32 mipCount |
//   per mip: u32 width, u32 height, u64 blockBytes, block bytes
// Written at import time when "bc7CacheDir" is present in the importer settings
// (AssetManager injects <LibraryRoot>/bc7). Returns false with *error set on failure.
bool WriteBcnFile(const std::filesystem::path& path, BcnFormat format,
                  const std::vector<std::pair<uint32_t, uint32_t>>& mipDimensions,
                  const std::vector<std::vector<uint8_t>>& mipBlocks, std::string* error = nullptr);

} // namespace raven
