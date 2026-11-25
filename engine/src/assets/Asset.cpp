#include "raven/assets/Asset.h"

namespace raven {

const char* ShaderStageToString(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Compute:  return "compute";
        default: return "unknown";
    }
}

ShaderStage ShaderStageFromString(const std::string& text) {
    if (text == "vertex" || text == "vert") return ShaderStage::Vertex;
    if (text == "fragment" || text == "frag") return ShaderStage::Fragment;
    if (text == "compute" || text == "comp") return ShaderStage::Compute;
    return ShaderStage::Vertex;
}

} // namespace raven
