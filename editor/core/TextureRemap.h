// RavenEditor — texture remap decisions, persisted in the model's .meta
// (EDITOR_DESIGN §10.4). Pure helpers, no ImGui — unit-testable.
#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "raven/assets/TextureNameRules.h"

namespace raven::editor {

// One user decision for one model slot. `chosen` is an assets-root-relative
// texture path, or empty for "explicitly skipped" (persisted as "" so the
// dialog never re-asks for a slot the user deliberately left blank).
struct RemapDecision {
    raven::TextureSlot slot = raven::TextureSlot::Diffuse;
    std::string chosen;
};

// Serialize to the .meta "textureRemap" object (slot name -> path).
nlohmann::json RemapToJson(const std::vector<RemapDecision>& decisions);

// Parse the .meta "textureRemap" object back into decisions. Unknown slot names
// are kept as-is but reported via outUnknown (usually a renamed/lost slot).
std::vector<RemapDecision> RemapFromJson(const nlohmann::json& json, std::vector<std::string>* outUnknown = nullptr);

// Apply persisted decisions onto fresh rows (prefill): returns map slot -> decision.
std::vector<std::pair<raven::TextureSlot, std::string>> DecisionsBySlot(const nlohmann::json& textureRemap);

} // namespace raven::editor
