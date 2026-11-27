#include "editor/core/TextureRemap.h"

namespace raven::editor {

nlohmann::json RemapToJson(const std::vector<RemapDecision>& decisions) {
    nlohmann::json out = nlohmann::json::object();
    for (const RemapDecision& decision : decisions) {
        if (decision.slot == raven::TextureSlot::kCount) continue;
        out[raven::SlotToString(decision.slot)] = decision.chosen;   // "" = explicit skip
    }
    return out;
}

std::vector<RemapDecision> RemapFromJson(const nlohmann::json& json, std::vector<std::string>* outUnknown) {
    std::vector<RemapDecision> out;
    if (!json.is_object()) return out;
    for (auto it = json.begin(); it != json.end(); ++it) {
        const raven::TextureSlot slot = raven::SlotFromString(it.key());
        if (slot == raven::TextureSlot::kCount) {
            if (outUnknown) outUnknown->push_back(it.key());
            continue;
        }
        RemapDecision decision;
        decision.slot = slot;
        decision.chosen = it.value().is_string() ? it.value().get<std::string>() : std::string();
        out.push_back(decision);
    }
    return out;
}

std::vector<std::pair<raven::TextureSlot, std::string>> DecisionsBySlot(const nlohmann::json& textureRemap) {
    std::vector<std::pair<raven::TextureSlot, std::string>> out;
    for (const RemapDecision& decision : RemapFromJson(textureRemap))
        out.emplace_back(decision.slot, decision.chosen);
    return out;
}

} // namespace raven::editor
