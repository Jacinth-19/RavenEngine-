// Render graph: compile order, pass culling, barrier planning, cycle detection.
#include "test_framework.h"

#include "raven/renderer/RenderGraph.h"

using namespace raven;

namespace {

RenderGraph BuildForwardGraph(bool shadowPass = true, bool grid = true) {
    RenderGraph graph;
    graph.AddResource({"shadow", GraphResourceType::DepthImage, 1024, 1024});
    graph.AddResource({"mainColor", GraphResourceType::ColorImage, 640, 480});
    graph.AddResource({"mainDepth", GraphResourceType::DepthImage, 640, 480});

    GraphPassDesc shadow;
    shadow.name = "shadow";
    shadow.enabled = shadowPass;
    shadow.writes = {{"shadow", GraphResourceState::DepthAttachmentWrite}};
    graph.AddPass(shadow);

    GraphPassDesc forward;
    forward.name = "forward";
    if (shadowPass) forward.reads = {{"shadow", GraphResourceState::SampledRead}};
    forward.writes = {{"mainColor", GraphResourceState::ColorAttachmentWrite},
                      {"mainDepth", GraphResourceState::DepthAttachmentWrite}};
    graph.AddPass(forward);

    if (grid) {
        GraphPassDesc g;
        g.name = "grid";
        g.sideEffect = true;
        g.reads = {{"mainColor", GraphResourceState::ColorAttachmentWrite}};
        g.writes = {{"mainColor", GraphResourceState::ColorAttachmentWrite}};
        graph.AddPass(g);
    }

    GraphPassDesc present;
    present.name = "present";
    present.sideEffect = true;
    present.reads = {{"mainColor", GraphResourceState::Present}};
    present.writes = {{"mainColor", GraphResourceState::Present}};
    graph.AddPass(present);
    graph.SetExternalOutput("mainColor");
    return graph;
}

} // namespace

RAVEN_TEST("render graph: shadow executes before forward, grid before present") {
    RenderGraph graph = BuildForwardGraph();
    CompiledGraph compiled = graph.Compile();
    CHECK(compiled.ok);

    std::vector<std::string> order;
    for (const GraphPassDesc* pass : compiled.executionOrder) order.push_back(pass->name);
    CHECK_EQ(order.size(), 4u);
    CHECK(order[0] == "shadow");
    CHECK(order[1] == "forward");
    CHECK(order[2] == "grid");
    CHECK(order[3] == "present");
}

RAVEN_TEST("render graph: disabled shadow pass is excluded and not reported as culled") {
    RenderGraph graph = BuildForwardGraph(/*shadowPass*/ false, /*grid*/ false);
    CompiledGraph compiled = graph.Compile();
    CHECK(compiled.ok);
    std::vector<std::string> order;
    for (const GraphPassDesc* pass : compiled.executionOrder) order.push_back(pass->name);
    CHECK_EQ(order.size(), 2u);
    CHECK(order[0] == "forward");
    CHECK(order[1] == "present");
    CHECK(compiled.culledPasses.empty());
}

RAVEN_TEST("render graph: barrier for shadow is emitted before the forward pass") {
    RenderGraph graph = BuildForwardGraph(/*shadowPass*/ true, /*grid*/ false);
    CompiledGraph compiled = graph.Compile();
    CHECK(compiled.ok);

    bool sawDepthToSampled = false;
    bool sawPresentTransition = false;
    for (const BarrierOp& op : compiled.barriers) {
        if (op.resource == "shadow" && op.from == GraphResourceState::DepthAttachmentWrite &&
            op.to == GraphResourceState::SampledRead) {
            CHECK(std::string(op.beforePass) == "forward");
            sawDepthToSampled = true;
        }
        if (op.resource == "mainColor" && op.from == GraphResourceState::ColorAttachmentWrite &&
            op.to == GraphResourceState::Present) {
            sawPresentTransition = true;
        }
    }
    CHECK(sawDepthToSampled);
    CHECK(sawPresentTransition);

    // shadow must start from Undefined (discard) before the shadow pass
    bool sawShadowInit = false;
    for (const BarrierOp& op : compiled.barriers) {
        if (op.resource == "shadow" && op.from == GraphResourceState::Undefined &&
            op.to == GraphResourceState::DepthAttachmentWrite) {
            sawShadowInit = true;
        }
    }
    CHECK(sawShadowInit);
}

RAVEN_TEST("render graph: pass writing an external output is never culled") {
    RenderGraph graph;
    graph.AddResource({"mainColor", GraphResourceType::ColorImage, 64, 64});
    GraphPassDesc fill;
    fill.name = "fill";
    fill.writes = {{"mainColor", GraphResourceState::ColorAttachmentWrite}};
    graph.AddPass(fill);   // sideEffect=false; only the external output keeps it alive
    graph.SetExternalOutput("mainColor");
    CompiledGraph compiled = graph.Compile();
    CHECK(compiled.ok);
    CHECK(compiled.culledPasses.empty());
    CHECK(compiled.executionOrder.size() == 1u);
}

RAVEN_TEST("render graph: read of a resource produced only by a later pass is rejected") {
    RenderGraph graph;
    graph.AddResource({"result", GraphResourceType::ColorImage, 64, 64});
    // Consumer declared BEFORE producer: with last-preceding-writer binding there is no
    // dependency, but the read would sample Undefined — reject it instead of scheduling garbage.
    GraphPassDesc consume;
    consume.name = "consume";
    consume.sideEffect = true;
    consume.reads = {{"result", GraphResourceState::SampledRead}};
    GraphPassDesc produce;
    produce.name = "produce";
    produce.sideEffect = true;
    produce.writes = {{"result", GraphResourceState::ColorAttachmentWrite}};
    graph.AddPass(consume);
    graph.AddPass(produce);
    CompiledGraph compiled = graph.Compile();
    CHECK(!compiled.ok);
    CHECK(compiled.error.find("produced later") != std::string::npos);
}

RAVEN_TEST("render graph: undeclared resource is rejected") {
    RenderGraph graph;
    GraphPassDesc p;
    p.name = "p";
    p.writes = {{"missing", GraphResourceState::ColorAttachmentWrite}};
    p.sideEffect = true;
    graph.AddPass(p);
    CompiledGraph compiled = graph.Compile();
    CHECK(!compiled.ok);
}

RAVEN_TEST("render graph: pass with no consumers and no side effects is culled") {
    RenderGraph graph;
    graph.AddResource({"scratch", GraphResourceType::ColorImage, 64, 64});
    graph.AddResource({"mainColor", GraphResourceType::ColorImage, 64, 64});
    GraphPassDesc dead;
    dead.name = "dead";
    dead.writes = {{"scratch", GraphResourceState::ColorAttachmentWrite}};
    graph.AddPass(dead);
    GraphPassDesc live;
    live.name = "live";
    live.sideEffect = true;
    live.writes = {{"mainColor", GraphResourceState::ColorAttachmentWrite}};
    graph.AddPass(live);
    CompiledGraph compiled = graph.Compile();
    CHECK(compiled.ok);
    CHECK_EQ(compiled.culledPasses.size(), 1u);
    CHECK(compiled.culledPasses[0] == "dead");
    CHECK_EQ(compiled.executionOrder.size(), 1u);
}
