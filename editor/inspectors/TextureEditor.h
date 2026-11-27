// RavenEditor — Texture resource editor (§6.3 resource mode; phase 3).
//
// Read-only facts (dimensions via stb_info, size on disk) + import settings
// (srgb / generateMipmaps / maxSize / compress) from the .meta `importSettings`
// section. Edits persist to the sidecar immediately and enqueue an async
// reimport through the import-progress tracker.
#pragma once

#include "raven/assets/AssetDatabase.h"

namespace raven::editor {

// `entry` must reference a live asset-database entry of type Texture.
void DrawTextureEditor(const raven::AssetDatabase::Entry& entry);

} // namespace raven::editor
