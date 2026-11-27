// RavenEditor — resource-mode Inspector entry point (§6.3).
//
// When an asset is selected in the project panel (independent of the entity
// selection), the Inspector switches to resource mode: header + a type-specific
// viewer (Material/Texture editors land here; other types fall back to a generic
// info block — Shader/PhysicsMaterial/Model/Font/Scene editors are Phase 6).
#pragma once

#include "raven/core/UUID.h"

namespace raven::editor {

class ResourceInspector {
public:
    // Draws the resource-mode Inspector for `asset`. `asset` must be non-nil and
    // resolvable in the asset database (missing → placeholder text).
    static void Draw(const raven::UUID& asset);
};

} // namespace raven::editor
