// RavenEditor — shared ASSET_UUID drag-drop payload (§5.4 / §10.1).
//
// ProjectPanel is the producer; hierarchy rows (model drop → instantiate child with
// MeshRendererComponent) and inspector asset slots (§6.3 consumers) accept the same
// bytes. Kept header-only so both sides agree on the wire format without a dep.
#pragma once

#include <cstdint>
#include <cstring>

#include "raven/assets/AssetTypes.h"   // AssetType
#include "raven/core/UUID.h"

namespace raven::editor {

struct AssetUuidPayload {
    uint64_t uuidHi = 0;
    uint64_t uuidLo = 0;
    uint32_t assetType = 0;   // raven::AssetType as uint32_t
    uint32_t reserved = 0;    // padding; keep 24 bytes, stable layout

    raven::UUID Uuid() const { return raven::UUID(uuidHi, uuidLo); }
    raven::AssetType Type() const { return static_cast<raven::AssetType>(assetType); }

    static AssetUuidPayload Make(const raven::UUID& uuid, raven::AssetType type) {
        AssetUuidPayload p;
        p.uuidHi = uuid.hi;
        p.uuidLo = uuid.lo;
        p.assetType = static_cast<uint32_t>(type);
        return p;
    }
};

// ImGui payload type tag.
inline constexpr char kAssetUuidPayloadType[] = "ASSET_UUID";

} // namespace raven::editor
