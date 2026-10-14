// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalSessionTestFixture.hpp"
#include "PhysicalGreedyScoring.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"
#include "TimingEditByteOrder.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "DataManager.hpp"
#include <algorithm>

namespace ista {
namespace {
using namespace timing_edit;
using namespace timing_edit::test;
using physical_test::require;
void addFanoutPort(physical_test::PhysicalDesign& physical)
{
  auto& database = STADM.getDatabase();
  auto extra = database.get_pin_map().at("out");
  extra.set_pin_name("out_keep"); extra.set_full_name("out_keep");
  database.get_pin_map().emplace("out_keep", std::move(extra));
  database.get_net_map().at("n0").get_pin_name_list().push_back("out_keep");
  auto* pin = physical.design.createOrFindIoPin("out_keep");
  pin->set_term()->set_direction(idb::IdbConnectDirection::kOutput);
  pin->set_average_coordinate(15000, 3000);
  require(physical.design.connectPinToNet(pin, physical.design.get_net_list()->find_net("n0")),
          "greedy fixture fanout connection failed");
  auto& child = database.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list().front().get_timing_arc_list().front();
  for (auto transition : {TransType::kRise, TransType::kFall}) {
    TimingTable delay, slew;
    delay.set_value_list({1E-9}); slew.set_value_list({0.07});
    child.get_delay_table_map()[transition] = delay;
    child.get_slew_table_map()[transition] = slew;
  }
}
void equalGainOrder(csr::detail::NativeLibraryModels& models, const LibraryCellRef& cell,
                    const StableNodeRef& first, const StableNodeRef& second)
{
  const std::array<std::string, 1> cells{cell.cell_name};
  const auto buffers = physical_greedy::buffers(STADM.getDatabase(), models, cells);
  physical_greedy::Net net{ExistingNetRef{"n0"}, {0, 0}, 0.07, 1.0,
      {{first, {1000, 2000}, -0.1, 0.2, 1.0}, {second, {1000, 2000}, -0.1, 0.2, 1.0}}};
  const auto selected = physical_greedy::score(net, buffers, editTestCreationToken(4), 1000);
  std::ranges::reverse(net.sinks);
  const auto reversed = physical_greedy::score(net, buffers, editTestCreationToken(4), 1000);
  require(selected && selected == reversed && selected->predicted_gain_ns > 0.0,
          "equal-gain sink order changed the stable choice");
  const auto expected = timing_edit::detail::canonicalBytesLess(canonicalStableNodeRefBytes(first),
      canonicalStableNodeRefBytes(second)) ? first : second;
  require(selected->edit.moved_sinks == std::vector<StableNodeRef>{expected},
          "equal-gain selection did not use the stable identity");
}
}
void physicalGreedyCandidateContract(bool with_cuda)
{
  std::vector<StaExecutionRequest> requests{
      {StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError},
      {StaBackend::kCpu, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError}};
  if (with_cuda) {
    requests.push_back({StaBackend::kCuda, StaUpdateStrategy::kFull, StaFallbackPolicy::kError});
    requests.push_back({StaBackend::kCuda, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError});
  }
  std::optional<PhysicalGreedySelection> expected;
  for (const auto request : requests) {
    NativeTransactionRuntime runtime;
    installNativeTransactionBaseline();
    physical_test::PhysicalDesign physical;
    physical_test::RecoveryDirectory recovery;
    addFanoutPort(physical);
    physical_test::constrainPorts(STADM.getDatabase());
    auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
    GuardedTestSession guarded(request, analysis);
    auto& session = guarded.session();
    const auto baseline = session.runPhysicalBaseline(recovery.config());
    if (!baseline.ok()) throw std::runtime_error(baseline.status.message);
    auto& database = STADM.getDatabase();
    const auto baseline_graph = std::get<csr::CsrGraphSnapshot>(csr::exportCsrGraphSnapshot(database));
    const auto node = [&](const std::string& name) {
      const auto found = std::ranges::find_if(baseline_graph.nodes, [&](const auto& value) { return value.key.full_name == name; });
      require(found != baseline_graph.nodes.end(), "candidate fixture port is missing");
      return StableNodeRef{ExistingNodeRef{found->key}};
    };
    const auto out = node("out"), keep = node("out_keep");
    const auto cell = nativeLibraryCellRef(database, "BUF_X1");
    const auto driver = editTestCreationToken(1);
    const auto old_native = csr::exportCsrGraphSnapshot(database);
    const bool csr_mode = request.backend == StaBackend::kCuda || request.update_strategy == StaUpdateStrategy::kIncremental;
    for (std::uint64_t before = 0; before != 3; ++before) {
      const auto token = editTestCreationToken(before + 1);
      const auto x = 5000 + static_cast<std::int32_t>(before) * 3000;
      BufferInsertionMutation edit{
          before == 0 ? StableNetRef{ExistingNetRef{"n0"}} : StableNetRef{CreatedNetRef{driver, CreatedNetRole::kOutputNet}},
          before == 0 ? std::vector<StableNodeRef>{out, keep} : std::vector<StableNodeRef>{before == 1 ? out : keep},
          cell, PlacedInputs{x / 1000.0, 7.0}, ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree}, token};
      const std::array locations{PhysicalBufferLocation{token, x, 7000, idb::IdbOrient::kN_R0}};
      const auto result = session.applyPhysicalBatch({TransactionGeneration{before}, {edit}}, locations);
      require(result.generation == before + 1 && !result.execution.fallback_to_full,
              "positive-gain fixture did not commit its requested batch");
    }
    for (std::uint64_t batch = 1; batch <= 3; ++batch) {
      const auto names = defaultTokenMaterialization(editTestCreationToken(batch), cell);
      auto* instance = physical.design.get_instance_list()->find_instance(names.instance_name);
      for (auto* pin : instance->get_pin_list()->get_pin_list()) {
        auto* xy = pin->get_average_coordinate();
        require(xy->get_x() >= 5000 && xy->get_y() >= 7000,
                "candidate fixture pin geometry was not placed");
      }
    }
    if (csr_mode) require(csr::exportCsrGraphSnapshot(database) == old_native, "candidate fixture eagerly rebuilt native");
    const auto selected = session.selectPhysicalBuffers(3, {"BUF_X1"}, 100, 100);
    require(selected.candidates.size() == 1 && selected.candidates.front().predicted_gain_ns > 0.0,
            "multi-sink fixture did not produce exactly one positive-gain candidate");
    require(selected == session.selectPhysicalBuffers(3, {"BUF_X1"}, 100, 100), "cached selection changed");
    if (expected) require(selected == *expected, "CSR positive-gain edit/score/target differs from independent native full");
    else expected = selected;
    if (csr_mode) require(csr::exportCsrGraphSnapshot(database) == old_native, "candidate selection materialized native timing");
    const auto reported = session.readPhysicalResult();
    const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session,
        [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
      auto& owner = StaTimingEditSessionProbeAccess::physicalState(context);
      owner.materialize(database, context, authority);
      const auto& resolver = owner.full ? owner.full->resolver() : owner.cpu ? owner.cpu->committed().resolver
#if ISTA_ENABLE_CUDA
          : owner.cuda_graph->resolver
#else
          : throw std::logic_error("candidate fixture has no owner")
#endif
          ;
      const std::array<std::string, 1> cells{"BUF_X1"};
      auto& models = context.libraryModels(database);
      require(selected == selectPhysicalBuffers(database, models, resolver, 3, cells, 100, 100, 1000),
              "positive-gain CSR selection differs after explicit materialization");
      const auto first = resolver.stableNodeForNativeName(defaultTokenMaterialization(editTestCreationToken(2), cell).instance_name + ":A");
      const auto second = resolver.stableNodeForNativeName(defaultTokenMaterialization(editTestCreationToken(3), cell).instance_name + ":A");
      equalGainOrder(models, cell, first, second);
      return StaStatus::success(session.state());
    });
    if (!status.ok()) throw std::runtime_error(status.message);
    const auto repeated = session.readPhysicalResult();
    require(repeated.generation == reported.generation && repeated.timing == reported.timing
                && repeated.qor_json == reported.qor_json && repeated.paths_json == reported.paths_json,
            "explicit native materialization changed the CSR public result");
    require(selected == session.selectPhysicalBuffers(3, {"BUF_X1"}, 100, 100),
            "explicit native materialization changed CSR selection");
    require(guarded.closeAndRelease().ok(), "positive-gain session close failed");
  }
}
}  // namespace ista
