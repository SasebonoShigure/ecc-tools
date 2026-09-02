// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrStructuredBatchTestSupport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrStateValidation.hpp"
#include "NativeDatabaseValidation.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "NarrowTimingRecordReader.hpp"
#include "Pin.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingCell.hpp"
#include "TimingPoint.hpp"

namespace ista::timing_dual_csr::test::structured_support {
namespace {

using timing_edit::AppendOnlyHighWater;
using timing_edit::CommittedEditIdentity;
using timing_edit::DesignEditPlan;
using timing_edit::ExistingNodeRef;
using timing_edit::NativeDatabaseCandidate;
using timing_edit::NativeDesignEditTranslator;
using timing_edit::TimingEditResolverState;
using timing_edit::TimingEditSessionToken;
using timing_edit::TransactionGeneration;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template <typename T>
csr::CsrBufferExtent extentOf(const std::vector<T>& values)
{
  return {static_cast<std::uint64_t>(values.size()),
          static_cast<std::uint64_t>(values.capacity())};
}

void addFanoutSink(Database& database, const std::string& sink_name)
{
  Pin sink = database.get_pin_map().at("out");
  sink.set_pin_name(sink_name);
  sink.set_full_name(sink_name);
  require(database.get_pin_map()
              .emplace(sink_name, std::move(sink))
              .second,
          "fanout sink pin collided with fixture");

  TimingPoint point = database.get_timing_point_map().at("out");
  require(database.get_timing_point_map()
              .emplace(sink_name, std::move(point))
              .second,
          "fanout sink timing point collided with fixture");
  database.get_end_point_list().push_back(sink_name);
  database.get_timing_order_list().push_back(sink_name);

  auto& net = database.get_net_map().at("n0");
  net.get_pin_name_list().push_back(sink_name);
  net.get_load_pin_list().push_back(sink_name);

  Arc arc = database.get_arc_list().front();
  const std::string source = arc.get_source_pin();
  arc.set_arc_name("n0:" + source + "->" + sink_name);
  arc.set_sink_pin(sink_name);
  database.get_arc_list().push_back(std::move(arc));
  const std::size_t index = database.get_arc_list().size() - 1;
  database.get_outgoing_arc_list_map().at(source).push_back(index);
  database.get_incoming_arc_list_map()[sink_name].push_back(index);

  const auto failure = timing_edit::validateNativeDatabase(database);
  require(!failure, failure ? "second-sink native fixture is invalid: "
                                  + failure->object + ": "
                                  + failure->detail
                            : std::string{});
}

csr::CsrReferenceInput exportReferenceInput(
    const Database& database, TimingEditSessionToken token)
{
  auto exported = csr::exportCsrReferenceInputCandidate(
      database, timing_edit::test::editTestAnalysisContext(),
      token.value());
  auto* input = std::get_if<csr::CsrReferenceInput>(&exported);
  require(input != nullptr,
          "structured baseline reference export failed");
  return std::move(*input);
}

DualCsrState makeBaselineGraph(
    const csr::CsrReferenceInput& input, TimingEditSessionToken token,
    const AppendOnlyHighWater& high_water)
{
  auto attempt = DualCsrBaselineFactory::create(
      input, token, high_water);
  require(attempt.success(),
          "structured Dual-CSR baseline was rejected"
              + (attempt.violations.empty()
                     ? std::string{}
                     : ": " + attempt.violations.front().field + " "
                           + attempt.violations.front().message));
  return std::move(*attempt.state);
}

csr::NodeKey nodeKeyByName(const DualCsrState& graph,
                           std::string_view name)
{
  const auto found = std::ranges::find(
      graph.nodes, name,
      [](const csr::NodeRecord& node) -> std::string_view {
        return node.key.full_name;
      });
  require(found != graph.nodes.end(),
          "Dual-CSR fixture is missing node " + std::string{name});
  return found->key;
}

csr::ArcId arcIdBySink(const DualCsrState& graph,
                       std::string_view sink)
{
  for (std::size_t index = 0; index < graph.arc_slots.size(); ++index)
    if (graph.arc_slots[index].active
        && graph.arc_slots[index].payload.kind == csr::ArcKind::kNet
        && graph.arc_slots[index].payload.key.base.sink.full_name == sink)
      return csr::ArcId{static_cast<std::uint64_t>(index)};
  throw std::runtime_error("Dual-CSR fixture is missing net arc to "
                           + std::string{sink});
}

void moveArcFromBaseToAux(DualCsrState& graph, csr::ArcId arc_id)
{
  const csr::NodeId source = graph.arc_slots.at(
      static_cast<std::size_t>(arc_id.value())).payload.source;
  const auto physical = std::ranges::find(
      graph.base_out.physical_slots,
      std::optional<csr::ArcId>{arc_id});
  require(physical != graph.base_out.physical_slots.end(),
          "Aux fixture arc is absent from BaseOut");
  physical->reset();

  graph.aux_out.row_offsets = {0, 1};
  graph.aux_out.row_sources = {source};
  graph.aux_out.arc_ids = {arc_id};
  graph.aux_out.source_to_aux_row.assign(graph.nodes.size(), -1);
  graph.aux_out.source_to_aux_row.at(
      static_cast<std::size_t>(source.value())) = 0;
  graph.aux_out.offsets_extent
      = extentOf(graph.aux_out.row_offsets);
  graph.aux_out.source_extent
      = extentOf(graph.aux_out.row_sources);
  graph.aux_out.edge_extent = extentOf(graph.aux_out.arc_ids);
  graph.aux_out.mapping_extent
      = extentOf(graph.aux_out.source_to_aux_row);

  graph.telemetry.base_active_edges
      = static_cast<std::uint64_t>(std::ranges::count_if(
          graph.base_out.physical_slots,
          [](const auto& slot) { return slot.has_value(); }));
  graph.telemetry.base_tombstones
      = static_cast<std::uint64_t>(
            graph.base_out.physical_slots.size())
        - graph.telemetry.base_active_edges;
  graph.telemetry.aux_rows = 1;
  graph.telemetry.aux_edges = 1;
  const auto violations = validateDualCsrState(graph);
  require(violations.empty(),
          "legal Base/Aux committed variant was rejected"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field + " "
                           + violations.front().message));
}

DualCsrArcGroupIndex makeArcGroupIndex(
    const DualCsrState& graph,
    const TimingEditResolverState& resolver)
{
  std::vector<DualCsrArcGroupIndexBinding> bindings;
  bindings.reserve(resolver.arcBindings().size());
  for (const auto& [ref, binding] : resolver.arcBindings())
    bindings.push_back({csr::ArcId{binding.slot.value()}, ref});
  std::ranges::sort(
      bindings, [](const auto& lhs, const auto& rhs) {
        return lhs.id.value() < rhs.id.value();
      });
  auto attempt = buildDualCsrArcGroupIndex(graph, bindings);
  require(attempt.success(),
          "structured fixture arc-group index was rejected"
              + (attempt.violations.empty()
                     ? std::string{}
                     : ": " + attempt.violations.front().field + " "
                           + attempt.violations.front().message));
  return std::move(*attempt.index);
}

CandidateBoundCapture captureCandidate(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture, PendingStructuredBatch& pending)
{
  std::optional<CandidateBoundCapture> captured;
  const StaStatus guarded
      = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
          session.session(),
          [&](std::uint64_t, StaCsrSessionContext& context,
              const timing_edit::NativeExecutionAuthority& authority) {
            timing_edit::test::requireReleasedSnapshotForNativeMutation(
                context, authority.sessionToken());
            NativeDatabaseCandidate candidate
                = NativeDatabaseCandidate::create(STADM.getDatabase());
            const auto translated
                = NativeDesignEditTranslator::applyNormalizedBatch(
                    candidate.database(), pending.preparedBatch(),
                    pending.stagedResolver());
            require(translated.applied() && translated.reasons.empty(),
                    "pure prepared batch did not translate on candidate");
            timing_edit::InstalledNativeDatabase installed
                = timing_edit::NativeCandidateExecutor::installAndExecute(
                    authority, std::move(candidate));
            const DualSemanticReadView committed(fixture.graph);
            captured = NarrowTimingRecordReader{}.read(
                installed.database(), committed,
                fixture.arc_group_index, pending.stagedResolver(),
                pending.preparedBatch(),
                pending.capturePlan());
            require(captured->success(),
                    "real narrow candidate capture was rejected");
            require(installed.rollback(),
                    "real narrow candidate did not roll back cleanly");
            return StaStatus::success(session.session().state());
          });
  require(guarded.ok(), "guarded real capture failed: "
                            + guarded.message);
  require(captured.has_value(),
          "guarded real capture returned no result");
  return std::move(*captured);
}

}  // namespace

static StructuredFixture makeStructuredFixtureImpl(
    Database& database, TimingEditSessionToken token,
    bool include_unmoved_fanout)
{
  addFanoutSink(database, "z");
  if (include_unmoved_fanout) {
    addFanoutSink(database, "stay");
    database.get_timing_library()
        .get_cell_map().at("BUF_X1")
        .get_port_map().at("A")
        .set_capacitance(0.17);
  }
  TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(database);
  const CommittedEditIdentity identity
      = timing_edit::NativeTimingEditPreparer::captureCurrentIdentity(
          database, TransactionGeneration{0},
          timing_edit::test::editTestAnalysisContext(), token);
  const timing_edit::BaselineSemanticIdentity baseline{
      database.get_design_name(), identity.current_fingerprints,
      identity.canonical_pre_state};
  csr::CsrReferenceInput baseline_reference_input
      = exportReferenceInput(database, token);
  DualCsrState graph = makeBaselineGraph(
      baseline_reference_input, token, resolver.highWater());
  DesignEditPlan plan = timing_edit::test::bufferInsertionPlan(
      database, baseline, TransactionGeneration{0},
      timing_edit::test::editTestCreationToken());
  // GraphBuilder uses lexical native pin order (out, z), while canonical
  // NodeKey bytes compare the length-prefixed z before out. This naturally
  // makes the candidate FIFO opposite canonical ArcKey order.
  plan.edits.front().moved_sinks.insert(
      plan.edits.front().moved_sinks.begin(),
      ExistingNodeRef{nodeKeyByName(graph, "z")});

  const csr::ArcId base_arc = arcIdBySink(graph, "out");
  const csr::ArcId aux_arc = arcIdBySink(graph, "z");
  const std::optional<csr::ArcId> unmoved_fanout_arc
      = include_unmoved_fanout
            ? std::optional<csr::ArcId>{arcIdBySink(graph, "stay")}
            : std::nullopt;
  moveArcFromBaseToAux(graph, aux_arc);
  DualCsrArcGroupIndex arc_group_index
      = makeArcGroupIndex(graph, resolver);
  return {token, std::move(resolver), identity, std::move(plan),
          std::move(baseline_reference_input), std::move(graph),
          std::move(arc_group_index), base_arc, aux_arc,
          unmoved_fanout_arc};
}

StructuredFixture makeStructuredFixture(
    Database& database, TimingEditSessionToken token)
{
  return makeStructuredFixtureImpl(database, token, false);
}

StructuredFixture makeStructuredFanoutFixture(
    Database& database, TimingEditSessionToken token)
{
  return makeStructuredFixtureImpl(database, token, true);
}

PendingStructuredBatch prepare(const StructuredFixture& fixture)
{
  const DualSemanticReadView view(fixture.graph);
  return DualCsrStructuredBatchPreparer::prepare(
      fixture.plan, fixture.resolver, view, fixture.graph,
      fixture.identity, fixture.arc_group_index);
}

CandidateBoundCapture captureCandidateForContractTest(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture,
    PendingStructuredBatch& pending)
{
  return captureCandidate(session, fixture, pending);
}

VerifiedStructuredBatch captureAndBuildVerifiedBatch(
    timing_edit::test::GuardedTestSession& session,
    const StructuredFixture& fixture)
{
  auto pending = prepare(fixture);
  CandidateBoundCapture capture
      = captureCandidate(session, fixture, pending);
  return TimingEditDualCsrDescriptorBuilder::build(
      std::move(pending), std::move(capture));
}

}  // namespace ista::timing_dual_csr::test::structured_support
