#include "raven/assets/Artifact.h"
#include "raven/core/FileSystem.h"

#include <cstring>
#include <fstream>

namespace raven {

namespace {
constexpr char kMagic[8] = {'R', 'A', 'V', 'N', 'A', 'S', '0', '1'};
constexpr uint32_t kFormatVersion = 1;
}

bool WriteRasset(const std::filesystem::path& path, AssetType type, const UUID& uuid,
                 const std::vector<uint8_t>& payload, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write artifact '" + path.string() + "'";
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    auto writePod = [&out](const auto& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    writePod(kFormatVersion);
    uint32_t typeBits = static_cast<uint32_t>(type);
    writePod(typeBits);
    writePod(uuid.hi);
    writePod(uuid.lo);
    uint64_t payloadSize = payload.size();
    writePod(payloadSize);
    if (!payload.empty()) out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!out) {
        if (error) *error = "failed writing artifact '" + path.string() + "'";
        return false;
    }
    return true;
}

std::optional<RassetData> ReadRasset(const std::filesystem::path& path, std::string* error) {
    auto bytes = fs::ReadBinary(path, error);
    if (!bytes) return std::nullopt;
    if (bytes->size() < 8 + 4 + 4 + 8 + 8 + 8) {
        if (error) *error = "artifact too small: " + path.string();
        return std::nullopt;
    }
    const uint8_t* p = bytes->data();
    if (std::memcmp(p, kMagic, 8) != 0) {
        if (error) *error = "bad artifact magic: " + path.string();
        return std::nullopt;
    }
    size_t offset = 8;
    auto readPod = [&p, &offset, &bytes](auto& v) {
        std::memcpy(&v, p + offset, sizeof(v));
        offset += sizeof(v);
    };
    uint32_t version = 0, typeBits = 0;
    UUID uuid;
    uint64_t payloadSize = 0;
    readPod(version);
    readPod(typeBits);
    readPod(uuid.hi);
    readPod(uuid.lo);
    readPod(payloadSize);
    if (version != kFormatVersion) {
        if (error) *error = "unsupported artifact version " + std::to_string(version) + ": " + path.string();
        return std::nullopt;
    }
    if (offset + payloadSize > bytes->size()) {
        if (error) *error = "truncated artifact payload: " + path.string();
        return std::nullopt;
    }
    RassetData out;
    out.type = static_cast<AssetType>(typeBits);
    out.uuid = uuid;
    out.payload.assign(p + offset, p + offset + payloadSize);
    return out;
}

void PayloadWriter::WriteU32(uint32_t v) { WritePod(v); }
void PayloadWriter::WriteU64(uint64_t v) { WritePod(v); }
void PayloadWriter::WriteF32(float v) { WritePod(v); }
void PayloadWriter::WriteBytes(const void* data, size_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    m_Buffer.insert(m_Buffer.end(), b, b + size);
}

bool PayloadReader::ReadU32(uint32_t& v) { return ReadPod(v); }
bool PayloadReader::ReadU64(uint64_t& v) { return ReadPod(v); }
bool PayloadReader::ReadF32(float& v) { return ReadPod(v); }
bool PayloadReader::ReadBytes(void* out, size_t size) {
    if (!BytesLeft(size)) return false;
    std::memcpy(out, m_Data + m_Pos, size);
    m_Pos += size;
    return true;
}

} // namespace raven
