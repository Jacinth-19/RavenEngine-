// RavenEngine — render graph: declare passes + resources, the graph solves
// execution order, culls passes with no live consumers, and emits the exact
// set of resource state transitions (pipeline barriers) required between passes.
//
// This is deliberately free of any Vulkan types: it models resource states as an
// enum and produces an ordered list of BarrierOps. The renderer translates those
// into vkCmdPipelineBarrier2 calls. That keeps the scheduling logic unit-testable
// on any machine without a GPU.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace raven {

enum class GraphResourceState : uint8_t {
    Undefined = 0,
    General,
    ColorAttachmentWrite,
    DepthAttachmentWrite,
    SampledRead,
    TransferSrc,
    TransferDst,
    Present,
};
const char* GraphResourceStateToString(GraphResourceState state);

enum class GraphResourceType : uint8_t { ColorImage, DepthImage, Buffer };

struct GraphResourceDesc {
    std::string name;
    GraphResourceType type = GraphResourceType::ColorImage;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GraphRead {
    std::string resource;
    GraphResourceState state = GraphResourceState::SampledRead;
};

struct GraphWrite {
    std::string resource;
    GraphResourceState state = GraphResourceState::ColorAttachmentWrite;
};

struct GraphPassDesc {
    std::string name;
    std::vector<GraphRead> reads;
    std::vector<GraphWrite> writes;
    bool sideEffect = false;   // never culled (e.g. present)
    bool enabled = true;
};

struct BarrierOp {
    std::string resource;
    GraphResourceType type = GraphResourceType::ColorImage;
    GraphResourceState from = GraphResourceState::Undefined;
    GraphResourceState to = GraphResourceState::Undefined;
    std::string beforePass;    // pass that triggered this transition
};

struct CompiledGraph {
    bool ok = false;
    std::string error;
    std::vector<const GraphPassDesc*> executionOrder;
    std::vector<BarrierOp> barriers;
    std::vector<std::string> culledPasses;   // for debugging/profiling
};

class RenderGraph {
public:
    void AddResource(const GraphResourceDesc& desc);
    void AddPass(const GraphPassDesc& pass);
    // Marks a resource as externally consumed (e.g. the swapchain image).
    void SetExternalOutput(const std::string& resource);
    CompiledGraph Compile() const;
    void Reset();

private:
    std::vector<GraphResourceDesc> m_Resources;
    std::vector<GraphPassDesc> m_Passes;
    std::vector<std::string> m_ExternalOutputs;
};

} // namespace raven
