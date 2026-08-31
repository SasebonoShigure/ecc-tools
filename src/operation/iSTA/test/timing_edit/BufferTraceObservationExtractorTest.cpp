// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Database.hpp"
#include "GreedyBufferCandidateSelector.hpp"
#include "Instance.hpp"
#include "NativeBufferObservationExtractor.hpp"
#include "NativeDatabaseValidation.hpp"
#include "Pin.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"
#include "TimingPathState.hpp"
#include "TimingTable.hpp"

namespace {

using namespace ista;
using namespace ista::buffer_trace;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

constexpr const char kSinkNodeName[] = "u_sink:A";

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

NativeEditFixture makeObservationFixture()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  Database& database = fixture.database();
  auto sink_node = database.get_pin_map().extract("out");
  require(!sink_node.empty(), "observation fixture lost its sink pin");
  sink_node.key() = kSinkNodeName;
  Pin& sink = sink_node.mapped();
  sink.set_pin_name("A");
  sink.set_full_name(kSinkNodeName);
  sink.set_instance_name("u_sink");
  sink.set_direction(PinDirection::kInput);
  sink.set_is_port(false);
  require(database.get_pin_map().insert(std::move(sink_node)).inserted,
          "failed to rekey the observation sink pin");

  Net& net = database.get_net_map().at("n0");
  net.set_pin_name_list({"in", kSinkNodeName});
  net.set_load_pin_list({kSinkNodeName});
  Arc& net_arc = database.get_arc_list().front();
  net_arc.set_arc_name("n0:in->u_sink:A");
  net_arc.set_sink_pin(kSinkNodeName);
  auto incoming = database.get_incoming_arc_list_map().extract("out");
  require(!incoming.empty(), "observation fixture lost incoming adjacency");
  incoming.key() = kSinkNodeName;
  require(database.get_incoming_arc_list_map()
              .insert(std::move(incoming))
              .inserted,
          "failed to rekey incoming adjacency");
  auto timing_point = database.get_timing_point_map().extract("out");
  require(!timing_point.empty(), "observation fixture lost its timing point");
  timing_point.key() = kSinkNodeName;
  require(database.get_timing_point_map()
              .insert(std::move(timing_point))
              .inserted,
          "failed to rekey the sink timing point");
  database.get_end_point_list() = {kSinkNodeName};
  database.get_timing_order_list() = {"in", kSinkNodeName};

  Instance instance;
  instance.set_instance_id(0);
  instance.set_instance_name("u_sink");
  instance.set_cell_name("OR2_X1");
  instance.set_pin_name_list({kSinkNodeName});
  instance.set_data_pin_name(kSinkNodeName);
  instance.set_is_sequential(false);
  instance.set_is_clock_gating(false);
  require(database.get_instance_map()
              .emplace("u_sink", std::move(instance))
              .second,
          "failed to install the observation sink instance");

  auto& cells = database.get_timing_library().get_cell_map();
  cells.at("OR2_X1").get_port_map().at("A").set_capacitance(0.003);
  cells.at("BUF_X1").get_port_map().at("A").set_capacitance(0.002);
  TimingTable scalar_delay;
  scalar_delay.set_value_list({0.02});
  TimingArc& buffer_arc = cells.at("BUF_X1")
                              .get_cell_arc_list()
                              .front()
                              .get_timing_arc_list()
                              .front();
  buffer_arc.get_delay_table_map().emplace(TransType::kRise, scalar_delay);
  buffer_arc.get_delay_table_map().emplace(TransType::kFall, scalar_delay);

  TimingPoint& point = database.get_timing_point_map().at(kSinkNodeName);
  point.set_arrival(1.25);
  point.set_required(2.5);
  point.set_slack(1.25);
  require(!validateNativeDatabase(database).has_value(),
          "observation fixture is not a valid native Database");
  return fixture;
}

void installPathState(TimingPoint& point, AnalysisType analysis,
                      double arrival)
{
  TimingPathState state;
  state.set_arrival(arrival);
  point.get_path_state_map()[analysis][PathSourceType::kInput]
                            [TransType::kRise]
                            .emplace("data", std::move(state));
}

std::vector<BufferCandidateObservation> extract(
    Database& database, const TimingEditResolverState& resolver)
{
  return NativeBufferObservationExtractor::extract(
      database, resolver,
      frozenSelectionProfile(ProfileId::kEngineeringSmoke));
}

BufferCandidateObservation extractOne(Database& database)
{
  const TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(database);
  auto observations = extract(database, resolver);
  require(observations.size() == 1,
          "fixture did not produce exactly one legal observation");
  return observations.front();
}

CandidateRejectionReason selectRejection(
    BufferCandidateObservation observation)
{
  GreedySelectionRequest request{
      .profile = frozenSelectionProfile(ProfileId::kEngineeringSmoke),
      .case_id = "extractor-direct",
      .seed = 42,
      .batch_ordinal = 1,
      .max_acceptances = 1,
      .candidates = {std::move(observation)},
      .reserved_creation_tokens = {},
      .reserved_materialized_names = {}};
  GreedySelectionResult result = GreedyBufferCandidateSelector{}.select(request);
  const auto* selected = std::get_if<GreedySelection>(&result);
  require(selected != nullptr && selected->receipt.evaluations.size() == 1,
          "selector rejected the direct extractor request");
  const auto& evaluation = selected->receipt.evaluations.front();
  require(evaluation.decision == CandidateDecision::kRejected
              && evaluation.rejection_reason.has_value(),
          "selector did not emit one typed rejection");
  return *evaluation.rejection_reason;
}

void testMaterializedMaxSetupAndBufferIdentity()
{
  NativeEditFixture fixture = makeObservationFixture();
  installPathState(fixture.database().get_timing_point_map().at(kSinkNodeName),
                   AnalysisType::kMax, 1.25);
  const BufferCandidateObservation observation
      = extractOne(fixture.database());
  require(observation.setup_timing
              == MaterializedSetupTiming{1.25, 2.5, 1.25},
          "max setup timing did not use the aggregate scalar projection");
  require(observation.moved_sinks
              == std::vector<StableNodeRef>{observation.candidate_pin},
          "extractor did not move exactly its single candidate sink");
  const auto* sink
      = std::get_if<ExistingNodeRef>(&observation.candidate_pin);
  require(sink != nullptr && sink->baseline_key.full_name == kSinkNodeName
              && sink->baseline_key.instance_name == "u_sink"
              && sink->baseline_key.pin_name == "A",
          "candidate sink identity differs from the native instance pin");
  require(observation.buffer.library_name == "slow.lib"
              && observation.buffer.cell_name == "BUF_X1"
              && observation.buffer.input_port == "A"
              && observation.buffer.output_port == "Y"
              && observation.buffer.ordered_arc_signatures.size() == 1,
          "legal buffer identity or arc signature differs");
  require(observation.candidate_load_pf == 0.003
              && observation.buffer_capacitance_pf == 0.002
              && observation.buffer_delay_ns == 0.02
              && std::isfinite(observation.buffer_delay_ns),
          "extractor did not use native pin capacitance and timing table");
  require(std::holds_alternative<UnplacedInputs>(observation.placement)
              && std::holds_alternative<NoParasiticRcInputs>(observation.rc),
          "extractor changed the frozen environment tags");
}

void testMissingMinOnlyAndDefaultTimingAreNotMaterialized()
{
  for (int scenario = 0; scenario < 3; ++scenario) {
    NativeEditFixture fixture = makeObservationFixture();
    TimingPoint& point
        = fixture.database().get_timing_point_map().at(kSinkNodeName);
    if (scenario == 1) {
      installPathState(point, AnalysisType::kMin, 1.25);
    } else if (scenario == 2) {
      installPathState(point, AnalysisType::kMax, 0.5);
      point.set_arrival(0.0);
      point.set_required(0.0);
      point.set_slack(0.0);
    }
    BufferCandidateObservation observation = extractOne(fixture.database());
    require(!observation.setup_timing.has_value(),
            "missing/min-only/default aggregate timing was materialized");
    require(selectRejection(std::move(observation))
                == CandidateRejectionReason::kTimingNotMaterialized,
            "unmaterialized timing returned the wrong selector reason");
  }
}

void testNonfiniteAggregateTimingIsTyped()
{
  NativeEditFixture fixture = makeObservationFixture();
  TimingPoint& point
      = fixture.database().get_timing_point_map().at(kSinkNodeName);
  installPathState(point, AnalysisType::kMax, 1.25);
  point.set_arrival(std::numeric_limits<double>::infinity());
  BufferCandidateObservation observation = extractOne(fixture.database());
  require(observation.setup_timing.has_value(),
          "finite max path did not expose its nonfinite aggregate timing");
  require(selectRejection(std::move(observation))
              == CandidateRejectionReason::kNonFiniteTiming,
          "nonfinite aggregate timing returned the wrong selector reason");
}

void testClockSequentialAndClockPortFiltering()
{
  {
    NativeEditFixture fixture = makeObservationFixture();
    installPathState(
        fixture.database().get_timing_point_map().at(kSinkNodeName),
        AnalysisType::kMax, 1.25);
    const TimingEditResolverState resolver
        = TimingEditResolverState::bindBaseline(fixture.database());
    fixture.database()
        .get_timing_point_map()
        .at(kSinkNodeName)
        .set_is_clock_point(true);
    require(extract(fixture.database(), resolver).empty(),
            "clock timing point was not filtered");
  }
  {
    NativeEditFixture fixture = makeObservationFixture();
    const TimingEditResolverState resolver
        = TimingEditResolverState::bindBaseline(fixture.database());
    fixture.database().get_instance_map().at("u_sink").set_is_sequential(true);
    require(extract(fixture.database(), resolver).empty(),
            "sequential sink was not filtered");
  }
  {
    NativeEditFixture fixture = makeObservationFixture();
    const TimingEditResolverState resolver
        = TimingEditResolverState::bindBaseline(fixture.database());
    fixture.database()
        .get_timing_library()
        .get_cell_map()
        .at("OR2_X1")
        .get_port_map()
        .at("A")
        .set_is_clock(true);
    require(extract(fixture.database(), resolver).empty(),
            "clock library port was not filtered");
  }
}

void testInvalidBufferAndMalformedTableAreFailClosed()
{
  {
    NativeEditFixture fixture = makeObservationFixture();
    const TimingEditResolverState resolver
        = TimingEditResolverState::bindBaseline(fixture.database());
    fixture.database()
        .get_timing_library()
        .get_cell_map()
        .at("BUF_X1")
        .set_is_sequential(true);
    require(extract(fixture.database(), resolver).empty(),
            "sequential buffer cell was accepted");
  }
  {
    NativeEditFixture fixture = makeObservationFixture();
    const TimingEditResolverState resolver
        = TimingEditResolverState::bindBaseline(fixture.database());
    fixture.database()
        .get_timing_library()
        .get_cell_map()
        .at("BUF_X1")
        .get_cell_arc_list()
        .front()
        .get_timing_arc_list()
        .front()
        .set_time_unit_scale(0.0);
    try {
      static_cast<void>(extract(fixture.database(), resolver));
    } catch (const NativeObservationError& error) {
      require(error.code()
                  == NativeObservationFailureCode::kInvalidLibraryTable
                  && std::string(token(error.code()))
                         == "invalid_observation_library_table"
                  && std::string(error.what()).find(
                         "native buffer Liberty arc table export failed")
                         != std::string::npos,
              "malformed buffer table returned the wrong typed failure");
      return;
    }
    throw std::runtime_error("malformed buffer table leaked or was accepted");
  }
}

}  // namespace

int main()
{
  testMaterializedMaxSetupAndBufferIdentity();
  testMissingMinOnlyAndDefaultTimingAreNotMaterialized();
  testNonfiniteAggregateTimingIsTyped();
  testClockSequentialAndClockPortFiltering();
  testInvalidBufferAndMalformedTableAreFailClosed();
  return 0;
}
