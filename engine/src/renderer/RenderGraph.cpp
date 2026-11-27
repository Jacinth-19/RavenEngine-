#include "raven/renderer/RenderGraph.h"
#include "raven/core/Logger.h"

#include <algorithm>
#include <queue>
#include <set>
#include <limits>

namespace raven {

const char* GraphResourceStateToString(GraphResourceState state) {
    switch (state) {
        case GraphResourceState::Undefined:            return "undefined";
        case GraphResourceState::General:              return "general";
        case GraphResourceState::ColorAttachmentWrite: return "color-attachment";
        case GraphResourceState::DepthAttachmentWrite: return "depth-attachment";
        case GraphResourceState::SampledRead:          return "sampled-read";
        case GraphResourceState::TransferSrc:          return "transfer-src";
        case GraphResourceState::TransferDst:          return "transfer-dst";
        case GraphResourceState::Present:              return "present";
        default: return "unknown";
    }
}

void RenderGraph::AddResource(const GraphResourceDesc& desc) { m_Resources.push_back(desc); }
void RenderGraph::AddPass(const GraphPassDesc& pass) { m_Passes.push_back(pass); }
void RenderGraph::SetExternalOutput(const std::string& resource) { m_ExternalOutputs.push_back(resource); }
void RenderGraph::Reset() {
    m_Resources.clear();
    m_Passes.clear();
    m_ExternalOutputs.clear();
}

static const GraphResourceDesc* FindResource(const std::vector<GraphResourceDesc>& resources, const std::string& name) {
    for (const auto& r : resources)
        if (r.name == name) return &r;
    return nullptr;
}

CompiledGraph RenderGraph::Compile() const {
    CompiledGraph result;

    // Validate references + declaration order (declaration order defines execution order).
    for (size_t i = 0; i < m_Passes.size(); ++i) {
        if (!m_Passes[i].enabled) continue;
        for (const auto& rd : m_Passes[i].reads) {
            if (!FindResource(m_Resources, rd.resource)) {
                result.error = "pass '" + m_Passes[i].name + "' reads undeclared resource '" + rd.resource + "'";
                return result;
            }
            bool anyEarlier = false, anyLater = false;
            size_t later = m_Passes.size();
            for (size_t k = 0; k < m_Passes.size(); ++k) {
                if (!m_Passes[k].enabled || k == i) continue;
                for (const auto& wr : m_Passes[k].writes) {
                    if (wr.resource != rd.resource) continue;
                    if (k < i) anyEarlier = true;
                    else if (k > i) { anyLater = true; later = std::min(later, k); }
                }
            }
            if (!anyEarlier && anyLater) {
                result.error = "pass '" + m_Passes[i].name + "' reads resource '" + rd.resource +
                               "' produced later by pass '" + m_Passes[later].name +
                               "' (declare passes in execution order)";
                return result;
            }
        }
        for (const auto& wr : m_Passes[i].writes) {
            if (!FindResource(m_Resources, wr.resource)) {
                result.error = "pass '" + m_Passes[i].name + "' writes undeclared resource '" + wr.resource + "'";
                return result;
            }
        }
    }

    const size_t n = m_Passes.size();

    // Build producer[resource] = pass indices writing it; consumer edges producer -> consumer.
    std::unordered_map<std::string, std::vector<size_t>> producers;
    for (size_t i = 0; i < n; ++i) {
        if (!m_Passes[i].enabled) continue;
        for (const auto& wr : m_Passes[i].writes) producers[wr.resource].push_back(i);
    }

    // Cull: a pass is live if it is a side effect, writes an external output,
    // or writes a resource consumed by a live pass. Walk backwards from the roots.
    std::vector<char> live(n, 0);
    std::queue<size_t> work;
    auto markLive = [&](size_t passIndex) {
        if (live[passIndex]) return;
        live[passIndex] = 1;
        work.push(passIndex);
    };
    for (size_t i = 0; i < n; ++i) {
        if (!m_Passes[i].enabled) continue;
        if (m_Passes[i].sideEffect) markLive(i);
        for (const auto& wr : m_Passes[i].writes) {
            for (const auto& ext : m_ExternalOutputs) {
                if (wr.resource == ext) markLive(i);
            }
        }
    }
    // A read binds to the most recent preceding writer of the resource (load-op
    // semantics). Binding to ALL writers would create false cycles for passes that
    // both read and write the same image (e.g. a grid pass following forward).
    auto lastPrecedingWriter = [&](const std::string& resource, size_t consumer) -> ptrdiff_t {
        auto it = producers.find(resource);
        if (it == producers.end()) return -1;
        ptrdiff_t writer = -1;
        for (size_t p : it->second) {
            if (p < consumer) writer = static_cast<ptrdiff_t>(p);
            else break;
        }
        return writer;
    };

    while (!work.empty()) {
        size_t i = work.front();
        work.pop();
        for (const auto& rd : m_Passes[i].reads) {
            const ptrdiff_t writer = lastPrecedingWriter(rd.resource, i);
            if (writer >= 0) markLive(static_cast<size_t>(writer));
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (m_Passes[i].enabled && !live[i]) result.culledPasses.push_back(m_Passes[i].name);
    }

    // Topological sort of the live subgraph (Kahn's algorithm). Edges: producer -> consumer.
    std::vector<int> inDegree(n, 0);
    std::vector<std::vector<size_t>> outEdges(n);
    for (size_t i = 0; i < n; ++i) {
        if (!live[i]) continue;
        for (const auto& rd : m_Passes[i].reads) {
            const ptrdiff_t writer = lastPrecedingWriter(rd.resource, i);
            if (writer < 0) continue;
            const size_t p = static_cast<size_t>(writer);
            if (!live[p]) continue;
            outEdges[p].push_back(i);
            ++inDegree[i];
        }
    }

    std::queue<size_t> ready;
    for (size_t i = 0; i < n; ++i) {
        if (live[i] && inDegree[i] == 0) ready.push(i);
    }
    std::vector<size_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        size_t i = ready.front();
        ready.pop();
        order.push_back(i);
        for (size_t next : outEdges[i]) {
            if (--inDegree[next] == 0) ready.push(next);
        }
    }
    if (order.size() != static_cast<size_t>(std::count(live.begin(), live.end(), 1))) {
        result.error = "render graph contains a dependency cycle";
        return result;
    }

    // Barrier planning: track each resource's current state in execution order.
    std::unordered_map<std::string, GraphResourceState> currentStates;
    for (size_t i : order) {
        const GraphPassDesc& pass = m_Passes[i];
        auto require = [&](const std::string& resource, GraphResourceState required, GraphResourceType type) {
            GraphResourceState& cur = currentStates[resource];  // default Undefined
            if (cur != required) {
                result.barriers.push_back(BarrierOp{resource, type, cur, required, pass.name});
                cur = required;
            }
        };
        for (const auto& wr : pass.writes) {
            const auto* desc = FindResource(m_Resources, wr.resource);
            require(wr.resource, wr.state, desc ? desc->type : GraphResourceType::ColorImage);
        }
        for (const auto& rd : pass.reads) {
            const auto* desc = FindResource(m_Resources, rd.resource);
            require(rd.resource, rd.state, desc ? desc->type : GraphResourceType::ColorImage);
        }
    }

    result.executionOrder.reserve(order.size());
    for (size_t i : order) result.executionOrder.push_back(&m_Passes[i]);
    result.ok = true;
    return result;
}

} // namespace raven
