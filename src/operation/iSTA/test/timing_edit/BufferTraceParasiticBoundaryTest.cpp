// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrSessionSnapshotAdapter.hpp"
#include "Database.hpp"
#include "NativeDatabaseCandidate.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "ParasiticNet.hpp"
#include "TimingEditCoordinator.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void installSourceParasitics(Database& database)
{
  ParasiticNet net;
  net.set_net_name("n0");
  net.set_lumped_capacitance(0.25);
  auto& library = database.get_parasitic_library();
  library.set_spef_file_path("stage4_boundary.spef");
  library.set_capacitive_unit("PF");
  library.set_resistance_unit("OHM");
  require(library.get_net_map().emplace("n0", std::move(net)).second,
          "failed to install the source parasitic net");
}

void requireUntouchedDetachedCandidate(
    Database& database, const TokenMaterialization& names)
{
  require(database.get_instance_map().empty()
              && !database.get_instance_map().contains(names.instance_name),
          "detached candidate materialized an instance");
  const auto& pins = database.get_pin_map();
  require(pins.size() == 2 && pins.contains("in") && pins.contains("out"),
          "detached candidate changed the baseline pin inventory");
  const Pin& input = pins.at("in");
  const Pin& output = pins.at("out");
  require(input.get_full_name() == "in" && input.get_pin_name() == "in"
              && input.get_instance_name().empty()
              && input.get_net_name() == "n0" && input.get_is_port()
              && input.get_direction() == PinDirection::kInput
              && output.get_full_name() == "out"
              && output.get_pin_name() == "out"
              && output.get_instance_name().empty()
              && output.get_net_name() == "n0" && output.get_is_port()
              && output.get_direction() == PinDirection::kOutput,
          "detached candidate changed baseline pin fields");
  const auto& nets = database.get_net_map();
  require(nets.size() == 1 && nets.contains("n0")
              && !nets.contains(names.output_net_name),
          "detached candidate changed the baseline net inventory");
  const Net& net = nets.at("n0");
  require(net.get_net_name() == "n0"
              && net.get_pin_name_list()
                     == std::vector<std::string>{"in", "out"}
              && net.get_driver_pin().empty()
              && net.get_driver_pin_list().empty()
              && net.get_load_pin_list().empty(),
          "detached candidate changed raw net membership or reset state");
  const auto& parasitics = database.get_parasitic_library();
  require(parasitics.get_spef_file_path() == "stage4_boundary.spef"
              && parasitics.get_capacitive_unit() == "PF"
              && parasitics.get_resistance_unit() == "OHM"
              && parasitics.get_net_map().size() == 1
              && parasitics.get_net_map().contains("n0")
              && parasitics.get_net_map().at("n0").get_net_name() == "n0"
              && parasitics.get_net_map().at("n0").get_lumped_capacitance()
                     == 0.25,
          "detached candidate changed source parasitic fields");
  require(database.get_arc_list().empty()
              && database.get_outgoing_arc_list_map().empty()
              && database.get_incoming_arc_list_map().empty()
              && database.get_timing_point_map().empty()
              && database.get_start_point_list().empty()
              && database.get_end_point_list().empty()
              && database.get_timing_order_list().empty()
              && database.get_timing_path_group_list().empty(),
          "detached candidate repopulated derived timing state");
}

struct SessionSignalRecorder
{
  SessionTransactionState state{SessionTransactionState::kBaselineExecuted};
  std::uint64_t writes{0};
};

void recordSessionState(void* context, SessionTransactionState state) noexcept
{
  auto& recorder = *static_cast<SessionSignalRecorder*>(context);
  recorder.state = state;
  ++recorder.writes;
}

void testRealParasiticsRequireRecordedFullWithoutPublication()
{
  constexpr TimingEditSessionToken kSession{91};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  installSourceParasitics(fixture.database());
  auto analysis_context = editTestAnalysisContext();
  analysis_context.rc_corner = "stage4_boundary_rc";

  const CommittedEditIdentity initial_identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), kGeneration, analysis_context, kSession);
  const BaselineSemanticIdentity baseline{
      fixture.database().get_design_name(), initial_identity.current_fingerprints,
      initial_identity.canonical_pre_state};
  TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(fixture.database());
  DesignEditPlan plan = bufferInsertionPlan(
      fixture.database(), baseline, kGeneration, editTestCreationToken());
  plan.edits.front().rc_inputs
      = ParasiticRcInputs{ParasiticEditPolicy::kFullWithRecord};

  PreparedBatch prepared = NativeTimingEditPreparer::prepare(
      fixture.database(), std::move(plan),
      TimingEditPrepareContext{kSession, baseline, initial_identity,
                               analysis_context},
      resolver);
  require(prepared.policyReceipt().disposition
              == EditDisposition::kFullWithRecord
              && prepared.policyReceipt().reasons
                     == std::vector<std::string>{
                         "source net parasitics require a recorded full rebuild"},
          "real parasitics did not produce the exact full_with_record receipt");

  SessionSignalRecorder session;
  TimingEditCoordinator coordinator(
      kSession, SessionStateSignal{&session, &recordSessionState}, baseline,
      initial_identity, CumulativeEditTrace{}, resolver);
  const TransactionStatus incremental_status
      = coordinator.beginPrepare(prepared);
  require(!incremental_status.ok()
              && coordinator.state() == TimingEditCoordinatorState::kIdle
              && coordinator.committedIdentity() == initial_identity
              && coordinator.cumulativeTrace().batches.empty()
              && coordinator.deviceState().generation == kGeneration
              && coordinator.commitMarker().generation == kGeneration
              && coordinator.resolverState() == resolver && session.writes == 0,
          "full_with_record policy entered the incremental coordinator");
  const TimingEditResolverState staged = resolver.stage(prepared);

  NativeDatabaseCandidate candidate
      = NativeDatabaseCandidate::create(fixture.database());
  const TokenMaterialization& names = prepared.tokenMaterializations().front();
  requireUntouchedDetachedCandidate(candidate.database(), names);
  const NativeTranslationResult translated
      = NativeDesignEditTranslator::applyNormalizedBatch(
          candidate.database(), prepared, staged);
  require(translated.disposition == EditDisposition::kFullWithRecord
              && translated.reasons == prepared.policyReceipt().reasons,
          "translator did not preserve the recorded-full policy receipt");
  requireUntouchedDetachedCandidate(candidate.database(), names);

  const CommittedEditIdentity final_identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), kGeneration, analysis_context, kSession);
  require(coordinator.state() == TimingEditCoordinatorState::kIdle
              && coordinator.committedIdentity() == initial_identity
              && coordinator.cumulativeTrace().batches.empty()
              && coordinator.deviceState().generation == kGeneration
              && coordinator.commitMarker().generation == kGeneration
              && coordinator.resolverState() == resolver
              && session.writes == 0 && final_identity == initial_identity,
          "full_with_record boundary changed or published generation zero");
  const auto& source_parasitics
      = fixture.database().get_parasitic_library();
  require(fixture.database().get_instance_map().empty()
              && fixture.database().get_pin_map().size() == 2
              && fixture.database().get_net_map().size() == 1
              && fixture.database().get_net_map().contains("n0")
              && !fixture.database().get_net_map().contains(names.output_net_name)
              && source_parasitics.get_spef_file_path()
                     == "stage4_boundary.spef"
              && source_parasitics.get_capacitive_unit() == "PF"
              && source_parasitics.get_resistance_unit() == "OHM"
              && source_parasitics.get_net_map().size() == 1
              && source_parasitics.get_net_map().contains("n0")
              && source_parasitics.get_net_map()
                         .at("n0")
                         .get_lumped_capacitance()
                     == 0.25,
          "full_with_record boundary mutated the source Database");
}

}  // namespace

int main()
{
  testRealParasiticsRequireRecordedFullWithoutPublication();
  return 0;
}
