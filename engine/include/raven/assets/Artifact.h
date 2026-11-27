// RavenEngine — .rasset binary artifact container (Library/Artifacts/<uuid>.rasset).
//
// Layout (little-endian):
//   char     magic[8]      "RAVNAS01"
//   uint32_t formatVersion (1)
//   uint32_t assetType     (AssetType enum)
//   uint64_t uuidHi
//   uint64_t uuidLo
//   uint64_t payloadBytes
//   uint8_t  payload[payloadBytes]
//
// Payloads are type-specific byte blobs produced by the importers (see Importers.h).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "raven/assets/AssetTypes.h"
#include "raven/core/UUID.h"

namespace raven {

struct RassetData {
    AssetType type = AssetType::Unknown;
    UUID uuid;
    std::vector<uint8_t> payload;
};

bool WriteRasset(const std::filesystem::path& path, AssetType type, const UUID& uuid,
                 const std::vector<uint8_t>& payload, std::string* error = nullptr);
std::optional<RassetData> ReadRasset(const std::filesystem::path& path, std::string* error = nullptr);

// Simple little-endian payload cursor used by the typed (de)serializers.
class PayloadWriter {
public:
    void WriteU32(uint32_t v);
    void WriteU64(uint64_t v);
    void WriteF32(float v);
    void WriteBytes(const void* data, size_t size);
    template <typename T> void WritePod(const T& v) { WriteBytes(&v, sizeof(T)); }
    const std::vector<uint8_t>& Data() const { return m_Buffer; }
private:
    std::vector<uint8_t> m_Buffer;
};

class PayloadReader {
public:
    PayloadReader(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}
    bool ReadU32(uint32_t& v);
    bool ReadU64(uint64_t& v);
    bool ReadF32(float& v);
    bool ReadBytes(void* out, size_t size);
    template <typename T> bool ReadPod(T& v) { return ReadBytes(&v, sizeof(T)); }
    bool BytesLeft(size_t needed) const { return m_Pos + needed <= m_Size; }
private:
    const uint8_t* m_Data;
    size_t m_Size;
    size_t m_Pos = 0;
};

} // namespace raven
