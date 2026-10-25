/** @brief Renderer FeatureGraph persistence and actual compiled Render provenance. */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "feature_graph_io.hpp"
#include "../Engine/RenderGraph/private/render_feature_projector.hpp"
#include "renderer_feature_profiles.hpp"

namespace {
using namespace aeg;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main() {
    try {
        feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        require(bool(renderer::register_scene_render_profiles(registry)), "real Renderer registration failed");
        const auto catalog = renderer::scene_render_profile_catalog();
        require(catalog && catalog.value.entries().size() == 3, "catalog contains technical Pipeline/Pass author entries");
        auto collision = catalog.value;
        require(!collision.add_entry(catalog.value.entries().front()) && collision.entries().size() == 3,
                "catalog accepted a duplicate Scene Feature definition");
        const auto root = std::filesystem::temp_directory_path() / "aeg_renderer_feature_authoring";
        std::filesystem::create_directories(root);
        const auto path = root / "Scene.aegfeature";
        for (auto profile : {renderer::SceneRenderProfile::kOpaque, renderer::SceneRenderProfile::kForward,
                             renderer::SceneRenderProfile::kDeferred}) {
            auto preset = renderer::scene_render_profile_graph(profile);
            require(bool(preset), "profile FeatureGraph factory failed");
            preset.value.graph_version = 17; preset.value.draft_revision = 9;
            require(bool(application::authoring::save_feature_graph_document(path, preset.value)), "FeatureGraph save failed");
            const auto loaded = application::authoring::load_feature_graph_document(path);
            require(loaded && loaded.value.asset_uuid == preset.value.asset_uuid && loaded.value.graph_version == 17 &&
                        loaded.value.draft_revision == 9 && loaded.value.contract_digest == catalog.value.contract_digest(),
                    "serialized graph lost author identity/version/digest");
            auto compiled = feature::FeatureGraphCompiler{}.compile(loaded.value, registry);
            feature::FeatureRuntime runtime(registry);
            require(compiled && compiled.value.plans.size() == 1 && compiled.value.provenance.size() == 1 &&
                        runtime.publish_graph(loaded.value, compiled.value) && runtime.tick(renderer::kSceneRenderProfileClock),
                    "persisted graph did not publish and execute its real Frame plan");
            const auto outputs = runtime.capture_render_outputs(compiled.value);
            require(outputs && outputs.value.size() == 1, "persisted graph has no actual captured Render output");
            const auto work = render_graph::RenderFeatureProjector{}.project(outputs.value, 73);
            require(work && work.value.size() == (profile == renderer::SceneRenderProfile::kOpaque ? 1u :
                                                 profile == renderer::SceneRenderProfile::kForward ? 5u : 4u),
                    "persisted profile lost its actual operation sequence");
            const auto& owner = compiled.value.provenance.front();
            for (size_t operation = 0; operation != work.value.size(); ++operation) {
                const auto& item = work.value[operation];
                require(item.node_id == owner.node_id && item.instance_id == owner.instance_id &&
                            item.feature_definition_id == owner.definition_id && item.owner_module == owner.owner_module &&
                            item.invocation == outputs.value.front().invocation && item.operation_index == operation &&
                            item.frame_index == 73, "projected work fabricated or dropped its Runtime source");
            }
            if (profile == renderer::SceneRenderProfile::kForward) {
                auto broken = outputs.value;
                std::swap(broken[0].intent.operations[2], broken[0].intent.operations[3]);
                require(!render_graph::RenderFeatureProjector{}.plan(broken), "private LightList consumer ran before its producer");
                auto foreign = outputs.value;
                std::get<feature::RenderDrawClusteredScene>(foreign[0].intent.operations[3]).light_list.id =
                    feature::RenderResourceId{"Unbound.LightList"};
                require(!render_graph::RenderFeatureProjector{}.plan(foreign), "unbound resource became executable");
            }
            auto empty = loaded.value;
            empty.nodes.clear(); empty.edges.clear(); empty.process_subgraphs.clear(); ++empty.graph_version;
            auto empty_plan = feature::FeatureGraphCompiler{}.compile(empty, registry);
            require(empty_plan && empty_plan.value.plans.empty() && runtime.publish_graph(empty, empty_plan.value),
                    "empty author graph revived hidden native profile work");
            const auto empty_outputs = runtime.capture_render_outputs(empty_plan.value);
            require(empty_outputs && empty_outputs.value.empty(), "empty author graph retained retired output");
            auto old_order = loaded.value;
            old_order.edges.push_back({old_order.nodes.front().node_id, feature::PortId{"BeforePass"},
                                       old_order.nodes.front().node_id, feature::PortId{"AfterPass"},
                                       feature::EdgeKind::kData, renderer::kSceneRenderProfileClock,
                                       renderer::kSceneRenderProfileClock, 1, 1});
            require(!feature::FeatureGraphCompiler{}.compile(old_order, registry), "technical Pass ordering became a valid Feature DataEdge");
        }
        for (auto old_payload : {"pipeline_nodes = []", "pass_nodes = []", "present_connections = []"}) {
            std::ofstream output(path);
            output << "schema_version = 1\ngraph_version = 1\nasset_uuid = \"renamed-retired\"\n" << old_payload << '\n';
            output.close();
            require(!application::authoring::load_feature_graph_document(path), "renamed retired payload passed the FeatureGraph reader");
        }
        auto backup = path;
        backup += ".bak";
        const auto previous = application::authoring::load_feature_graph_document(backup);
        const auto forward = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kForward);
        require(previous && forward && previous.value.asset_uuid == forward.value.asset_uuid &&
                    previous.value.graph_version == 17 && previous.value.draft_revision == 9 &&
                    previous.value.contract_digest == catalog.value.contract_digest(),
                "FeatureGraph replacement lost the complete previous Render profile backup");
        require(std::filesystem::remove(path), "FeatureGraph fixture source cleanup failed");
        require(std::filesystem::remove(backup), "FeatureGraph fixture backup cleanup failed");
        require(std::filesystem::remove(root), "FeatureGraph fixture directory cleanup failed");
        std::cout << "Renderer FeatureGraph authoring/persistence and actual Render provenance passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
