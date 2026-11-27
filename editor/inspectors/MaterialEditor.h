// RavenEditor — Material resource editor (§6.3 resource mode; phase 3).
//
// Edits the .rmat file in place: albedo/emissive color, metallic/roughness/
// normalScale scalars, and albedo/normal texture references (picked from the
// texture database). Live edits go through MaterialSnapshotCommand with
// PushCoalesced so one drag gesture == one undo entry; the applier rewrites the
// .rmat and reimports the asset in place (material parse is microseconds).
#pragma once

#include "raven/assets/AssetDatabase.h"

namespace raven::editor {

// `entry` must reference a live asset-database entry of type Material.
void DrawMaterialEditor(const raven::AssetDatabase::Entry& entry);

} // namespace raven::editor
