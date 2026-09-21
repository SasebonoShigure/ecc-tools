// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CsrTimingScatterTestFixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "AnalysisType.hpp"
#include "Arc.hpp"
#include "CsrFifoLevelization.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrTableLookup.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "PathSourceType.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingClock.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPathState.hpp"
#include "TimingPoint.hpp"
#include "TimingPortConstraint.hpp"
#include "TransType.hpp"

namespace ista::csr::test {
namespace {

[[nodiscard]] AnalysisType nativeAnalysis(AnalysisMode analysis)
{
  switch (analysis) {
    case AnalysisMode::kMax:
      return AnalysisType::kMax;
    case AnalysisMode::kMin:
      return AnalysisType::kMin;
  }
  throw std::runtime_error("timing scatter fixture: unknown analysis");
}

[[nodiscard]] TransType nativeTransition(Transition transition)
{
  switch (transition) {
    case Transition::kRise:
      return TransType::kRise;
    case Transition::kFall:
      return TransType::kFall;
    case Transition::kNone:
      return TransType::kNone;
  }
  throw std::runtime_error("timing scatter fixture: unknown transition");
}

[[nodiscard]] PathSourceType nativePathSource(PathSource source)
{
  switch (source) {
    case PathSource::kInput:
      return PathSourceType::kInput;
    case PathSource::kRegister:
      return PathSourceType::kRegister;
  }
  throw std::runtime_error("timing scatter fixture: unknown path source");
}

[[nodiscard]] TimingPoint& pointFor(Database& database, const CsrReferenceInput& input, NodeId id)
{
  if (id.value() >= input.nodes.size()) {
    throw std::runtime_error("timing scatter fixture: NodeId is out of range");
  }
  return database.get_timing_point_map().at(input.nodes[id.value()].key.full_name);
}

[[nodiscard]] Arc& arcFor(Database& database, const CsrReferenceInput& input, ArcId id)
{
  if (id.value() >= input.arcs.size()) {
    throw std::runtime_error("timing scatter fixture: ArcId is out of range");
  }
  const std::uint64_t rank = input.arcs[id.value()].native_traversal_rank;
  if (rank >= database.get_arc_list().size()) {
    throw std::runtime_error("timing scatter fixture: native Arc rank is out of range");
  }
  return database.get_arc_list()[rank];
}

[[nodiscard]] const ClockConstraintModel* directClock(const CsrReferenceInput& input, NodeId node)
{
  for (const ClockConstraintModel& clock : input.clocks) {
    for (const RawNodeReference& source : clock.ordered_sources) {
      if (source.resolved_node == node) {
        return &clock;
      }
    }
  }
  return nullptr;
}

void makeChainSupported(Database& database)
{
  for (auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(name);
    instance.set_is_sequential(false);
    instance.get_check_arc_list().clear();
    instance.set_clock_pin_name({});
    instance.set_data_pin_name({});
  }
  for (auto& [name, cell] : database.get_timing_library().get_cell_map()) {
    static_cast<void>(name);
    cell.set_is_sequential(false);
    cell.get_check_arc_list().clear();
    for (TimingCellArc& cell_arc : cell.get_cell_arc_list()) {
      for (TimingArc& child : cell_arc.get_timing_arc_list()) {
        child.set_sdf_cond({});
      }
    }
  }
  for (Arc& arc : database.get_arc_list()) {
    arc.set_is_disable_arc(false);
  }
  TimingClock& clock = database.get_timing_constraint().get_clock_map().at("a_clock_key");
  clock.set_source_list({"out"});
  clock.set_is_propagated(false);
  database.get_timing_constraint().get_case_analysis_map().clear();
  database.get_start_point_list() = {"clk", "din", "ff1:D"};
}

void resetPoint(TimingPoint& point, std::int32_t level)
{
  point.set_arrival(-std::numeric_limits<double>::infinity());
  point.set_required(std::numeric_limits<double>::infinity());
  point.set_slack(0.0);
  point.set_launch_time(0.0);
  point.set_level(level);
  point.set_is_clock_point(false);
  point.set_clock_name({});
  point.set_predecessor({});
  point.set_predecessor_arc_idx(std::numeric_limits<std::size_t>::max());
  point.get_clock_arrival_map().clear();
  point.get_clock_slew_map().clear();
  point.get_clock_predecessor_map().clear();
  point.get_clock_predecessor_arc_delay_map().clear();
  point.get_clock_predecessor_trans_type_map().clear();
  point.get_physical_clock_arrival_map().clear();
  point.get_physical_clock_slew_map().clear();
  point.get_physical_clock_predecessor_map().clear();
  point.get_physical_clock_predecessor_arc_delay_map().clear();
  point.get_physical_clock_predecessor_trans_type_map().clear();
  point.get_data_slew_map().clear();
  point.get_path_state_map().clear();
}

void seedIdealClocks(Database& database, const CsrReferenceInput& input)
{
  for (const ClockConstraintModel& clock : input.clocks) {
    for (const RawNodeReference& source : clock.ordered_sources) {
      if (!source.resolved_node) {
        continue;
      }
      TimingPoint& point = pointFor(database, input, *source.resolved_node);
      point.set_is_clock_point(true);
      point.set_clock_name(clock.map_key);
      for (AnalysisMode analysis : input.analysis_order) {
        for (Transition transition : input.transition_order) {
          const AnalysisType native_analysis = nativeAnalysis(analysis);
          const TransType native_transition = nativeTransition(transition);
          point.get_clock_arrival_map()[native_analysis][native_transition] = 0.0;
          point.get_clock_slew_map()[native_analysis][native_transition] = 0.0;
          point.get_physical_clock_arrival_map()[native_analysis][native_transition] = 0.0;
          point.get_physical_clock_slew_map()[native_analysis][native_transition] = 0.0;
        }
      }
    }
  }
}

void seedStarts(Database& database, const CsrReferenceInput& input)
{
  for (const StartSeedModel& seed : input.start_seeds) {
    TimingPoint& point = pointFor(database, input, seed.seed_node);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        if (!seed.slew[analysis][transition]) {
          throw std::runtime_error("timing scatter fixture: start seed slew is absent");
        }
        point.get_data_slew_map()[nativeAnalysis(input.analysis_order[analysis])][nativeTransition(input.transition_order[transition])]
            = *seed.slew[analysis][transition];
      }
    }

    point.set_arrival(0.0);
    if (seed.path_source) {
      const auto& scalar = seed.arrival[0][0];
      if (!scalar) {
        throw std::runtime_error("timing scatter fixture: start seed scalar arrival is absent");
      }
      point.set_arrival(*scalar);
    }
    const ClockConstraintModel* clock = directClock(input, seed.seed_node);
    point.set_launch_time(clock == nullptr ? 0.0 : clock->edges[0]);
    point.set_clock_name(seed.clock_tag);

    if (!seed.path_source) {
      continue;
    }
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        const auto& arrival = seed.arrival[analysis][transition];
        const auto& slew = seed.slew[analysis][transition];
        if (!arrival || !slew) {
          throw std::runtime_error("timing scatter fixture: materialized start lane is absent");
        }
        TimingPathState path;
        path.set_arrival(*arrival);
        path.set_slew(*slew);
        path.set_launch_time(clock == nullptr || !(analysis == 0 && transition == 1) ? (clock == nullptr ? 0.0 : clock->edges[0]) : clock->edges[1]);
        path.set_start_point(input.nodes.at(seed.path_start_node.value()).key.full_name);
        path.set_clock_name(seed.clock_tag);
        path.set_trans_type(nativeTransition(input.transition_order[transition]));
        if (seed.crpr_node) {
          path.set_crpr_clock_pin(input.nodes.at(seed.crpr_node->value()).key.full_name);
          path.set_crpr_clock_trans_type(nativeTransition(*seed.crpr_transition));
        }
        point.get_path_state_map()[nativeAnalysis(input.analysis_order[analysis])][nativePathSource(*seed.path_source)]
                                  [nativeTransition(input.transition_order[transition])][seed.clock_tag]
            = std::move(path);
      }
    }
  }
}

[[nodiscard]] std::size_t transitionLane(Transition transition)
{
  return transition == Transition::kRise ? 0 : 1;
}

[[nodiscard]] bool chainChildSelected(const ChildTimingArcModel& child, Transition input, Transition output)
{
  if (child.signature.sense != TimingSense::kPositiveUnate && child.signature.sense != TimingSense::kNoneUsesPositiveFallback) {
    throw std::runtime_error("timing scatter fixture: supported chain requires positive-unate children");
  }
  return input == output;
}

[[nodiscard]] bool chainRelationExists(const ArcRecord& record, Transition input, Transition output)
{
  if (record.transfer.is_clock_arc) {
    const Transition trigger = record.transfer.children().front().signature.trigger_transition;
    if (input != (trigger == Transition::kFall ? Transition::kFall : Transition::kRise)) {
      return false;
    }
  } else if (input != output) {
    return false;
  }
  const std::size_t lane = transitionLane(output);
  return std::ranges::any_of(record.transfer.children(), [lane](const ChildTimingArcModel& child) {
    return child.delay_tables[lane].present || child.slew_tables[lane].present || child.check_tables[lane].present;
  });
}

[[nodiscard]] double chainTableValue(const TimingTableModel& table, const ChildTimingArcModel& child, double slew, double load)
{
  const std::optional<double> value = lookupCsrTimingTable(table, slew, load, child.time_unit_scale, child.capacitance_unit_scale);
  if (!value || !std::isfinite(*value)) {
    throw std::runtime_error("timing scatter fixture: independent table lookup failed");
  }
  return *value;
}

[[nodiscard]] bool better(AnalysisMode analysis, double candidate, double current)
{
  return analysis == AnalysisMode::kMax ? candidate > current : candidate < current;
}

void seedArcs(Database& database, const CsrReferenceInput& input)
{
  for (const ArcRecord& record : input.arcs) {
    Arc& arc = arcFor(database, input, record.id);
    arc.get_trans_delay_map().clear();
    arc.get_input_output_delay_map().clear();
    arc.get_graph_delay_map().clear();
    arc.get_trans_type_map().clear();
    arc.replace_timing_arc_delay_state({});
    for (AnalysisMode analysis : input.analysis_order) {
      const std::size_t analysis_lane = analysis == AnalysisMode::kMax ? 0 : 1;
      const AnalysisType native_analysis = nativeAnalysis(analysis);
      for (Transition input_transition : input.transition_order) {
        const TransType native_input = nativeTransition(input_transition);
        arc.get_trans_delay_map()[native_analysis][native_input] = 0.0;
        if (record.kind == ArcKind::kNet || record.transfer.children().empty()) {
          const double delay = record.kind == ArcKind::kNet ? 0.0 : (analysis == AnalysisMode::kMax ? record.transfer.delay_max : record.transfer.delay_min);
          arc.get_trans_delay_map()[native_analysis][native_input] = delay;
          arc.get_input_output_delay_map()[native_analysis][native_input][native_input] = delay;
          arc.get_trans_type_map()[native_input] = native_input;
          continue;
        }

        bool winner_seen = false;
        double winner = 0.0;
        Transition winner_output = Transition::kNone;
        for (Transition output : input.transition_order) {
          if (!chainRelationExists(record, input_transition, output)) {
            continue;
          }
          const std::size_t output_lane = transitionLane(output);
          bool candidate_seen = false;
          double aggregate = 0.0;
          for (const ChildTimingArcModel& child : record.transfer.children()) {
            if (!chainChildSelected(child, input_transition, output) || !child.delay_tables[output_lane].present) {
              continue;
            }
            const double candidate = chainTableValue(child.delay_tables[output_lane], child, 0.0, record.output_load->at(analysis_lane).at(output_lane));
            arc.update_timing_arc_delay(child.signature.arc_index, native_analysis, native_input, nativeTransition(output), candidate, true);
            if (!candidate_seen || better(analysis, candidate, aggregate)) {
              aggregate = candidate;
              candidate_seen = true;
            }
          }
          if (!candidate_seen) {
            aggregate = record.transfer.delay;
          }
          arc.get_input_output_delay_map()[native_analysis][native_input][nativeTransition(output)] = aggregate;
          if (!winner_seen || better(analysis, aggregate, winner)) {
            winner = aggregate;
            winner_output = output;
            winner_seen = true;
          }
        }
        if (winner_seen) {
          arc.get_trans_delay_map()[native_analysis][native_input] = winner;
          arc.get_trans_type_map()[native_input] = nativeTransition(winner_output);
        }
      }
    }
    const auto& delays = std::as_const(arc).get_trans_delay_map();
    arc.set_delay_max(std::max(delays.at(AnalysisType::kMax).at(TransType::kRise), delays.at(AnalysisType::kMax).at(TransType::kFall)));
    arc.set_delay_min(std::min(delays.at(AnalysisType::kMin).at(TransType::kRise), delays.at(AnalysisType::kMin).at(TransType::kFall)));
    arc.set_delay(arc.get_delay_max());
  }
}

void propagateForward(Database& database, const CsrReferenceInput& input, const CsrFifoLevelization& fifo)
{
  constexpr double kWinnerEpsilon = 1.0e-6;
  for (NodeId source_id : fifo.level_nodes) {
    TimingPoint& source = pointFor(database, input, source_id);
    for (std::uint64_t offset = input.out_row_offsets.at(source_id.value()); offset < input.out_row_offsets.at(source_id.value() + 1); ++offset) {
      const ArcId arc_id = input.out_arc_ids.at(offset);
      const ArcRecord& record = input.arcs.at(arc_id.value());
      if (!record.eligibility.data_slew) {
        continue;
      }
      Arc& arc = arcFor(database, input, arc_id);
      TimingPoint& sink = pointFor(database, input, record.sink);
      for (const auto& [native_analysis, transitions] : source.get_data_slew_map()) {
        const AnalysisMode analysis = native_analysis == AnalysisType::kMax ? AnalysisMode::kMax : AnalysisMode::kMin;
        const std::size_t analysis_lane = analysis == AnalysisMode::kMax ? 0 : 1;
        for (const auto& [native_input, source_slew] : transitions) {
          const Transition input_transition = native_input == TransType::kRise ? Transition::kRise : Transition::kFall;
          for (Transition output : input.transition_order) {
            const TransType native_output = nativeTransition(output);
            const auto& io = std::as_const(arc).get_input_output_delay_map();
            if (!io.contains(native_analysis) || !io.at(native_analysis).contains(native_input)
                || !io.at(native_analysis).at(native_input).contains(native_output)) {
              continue;
            }
            double delay = io.at(native_analysis).at(native_input).at(native_output);
            double output_slew = source_slew;
            bool delay_seen = false;
            bool slew_seen = false;
            const std::size_t output_lane = transitionLane(output);
            for (const ChildTimingArcModel& child : record.transfer.children()) {
              if (!chainChildSelected(child, input_transition, output)) {
                continue;
              }
              const double load = record.output_load->at(analysis_lane).at(output_lane);
              if (child.delay_tables[output_lane].present) {
                const double candidate = chainTableValue(child.delay_tables[output_lane], child, source_slew, load);
                arc.update_timing_arc_delay(child.signature.arc_index, native_analysis, native_input, native_output, candidate, false);
                if (!delay_seen || better(analysis, candidate, delay)) {
                  delay = candidate;
                  delay_seen = true;
                }
              }
              if (child.slew_tables[output_lane].present) {
                const double candidate = chainTableValue(child.slew_tables[output_lane], child, source_slew, load);
                if (!slew_seen || better(analysis, candidate, output_slew)) {
                  output_slew = candidate;
                  slew_seen = true;
                }
              }
            }
            if (!record.transfer.children().empty() && !delay_seen) {
              delay = record.transfer.delay;
            }
            auto& graph = arc.get_graph_delay_map()[native_analysis][native_input];
            const auto graph_value = graph.find(native_output);
            if (graph_value == graph.end()
                || (analysis == AnalysisMode::kMax ? delay > graph_value->second + kWinnerEpsilon : delay < graph_value->second - kWinnerEpsilon)) {
              graph[native_output] = delay;
            }
            auto& sink_slews = sink.get_data_slew_map()[native_analysis];
            const auto sink_value = sink_slews.find(native_output);
            if (sink_value == sink_slews.end()
                || (analysis == AnalysisMode::kMax ? output_slew > sink_value->second + kWinnerEpsilon : output_slew < sink_value->second - kWinnerEpsilon)) {
              sink_slews[native_output] = output_slew;
            }
          }
        }
      }
    }
  }

  for (NodeId source_id : fifo.level_nodes) {
    TimingPoint& source = pointFor(database, input, source_id);
    for (std::uint64_t offset = input.out_row_offsets.at(source_id.value()); offset < input.out_row_offsets.at(source_id.value() + 1); ++offset) {
      const ArcRecord& record = input.arcs.at(input.out_arc_ids.at(offset).value());
      if (!record.eligibility.data_arrival || !std::isfinite(source.get_arrival())) {
        continue;
      }
      TimingPoint& sink = pointFor(database, input, record.sink);
      const double candidate = source.get_arrival() + arcFor(database, input, record.id).get_delay();
      if (!std::isfinite(sink.get_arrival()) || candidate > sink.get_arrival() + 1.0e-9) {
        sink.set_arrival(candidate);
        sink.set_launch_time(source.get_launch_time());
        sink.set_clock_name(source.get_clock_name());
        sink.set_predecessor(input.nodes.at(source_id.value()).key.full_name);
        sink.set_predecessor_arc_idx(record.native_traversal_rank);
      }
    }
  }
}

void propagateRequired(Database& database, const CsrReferenceInput& input, const CsrFifoLevelization& fifo)
{
  double default_required = 0.0;
  for (NodeId endpoint : input.ordered_end_ids) {
    const double arrival = pointFor(database, input, endpoint).get_arrival();
    if (std::isfinite(arrival)) {
      default_required = std::max(default_required, arrival);
    }
  }
  for (NodeId endpoint : input.ordered_end_ids) {
    pointFor(database, input, endpoint).set_required(default_required);
  }
  for (const EndpointRequiredModel& endpoint : input.endpoint_required) {
    if (endpoint.output_delay[0]) {
      pointFor(database, input, endpoint.node).set_required(endpoint.clock_period - *endpoint.output_delay[0]);
    }
  }

  for (auto node = fifo.level_nodes.rbegin(); node != fifo.level_nodes.rend(); ++node) {
    TimingPoint& source = pointFor(database, input, *node);
    for (std::uint64_t offset = input.out_row_offsets.at(node->value()); offset < input.out_row_offsets.at(node->value() + 1); ++offset) {
      const ArcRecord& record = input.arcs.at(input.out_arc_ids.at(offset).value());
      if (!record.eligibility.data_arrival) {
        continue;
      }
      const double candidate = pointFor(database, input, record.sink).get_required() - arcFor(database, input, record.id).get_delay();
      if (std::isfinite(candidate) && (!std::isfinite(source.get_required()) || candidate < source.get_required())) {
        source.set_required(candidate);
      }
    }
  }
  for (const NodeRecord& node : input.nodes) {
    TimingPoint& point = pointFor(database, input, node.id);
    const double slack = std::isfinite(point.get_arrival()) && std::isfinite(point.get_required()) ? point.get_required() - point.get_arrival() : 0.0;
    point.set_slack(slack);
  }
}

void seedIndependentNativeTiming(Database& database, const CsrReferenceInput& input)
{
  const std::vector<CsrContractViolation> violations = validateReferenceInput(input);
  if (!violations.empty()) {
    throw std::runtime_error("timing scatter fixture: strict input violation at " + violations.front().field + ": " + violations.front().message);
  }
  if (!input.unsupported_reasons.empty()) {
    throw std::runtime_error("timing scatter fixture: unsupported input reason " + std::to_string(static_cast<unsigned>(input.unsupported_reasons.front())));
  }
  const CsrFifoLevelization fifo = replayFifoLevelization(input);
  if (!fifo.complete) {
    throw std::runtime_error("timing scatter fixture: independent seeder requires an acyclic graph");
  }
  for (const NodeRecord& node : input.nodes) {
    const std::uint64_t level = fifo.node_to_level.at(node.id.value());
    if (level == 0 || level > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::runtime_error("timing scatter fixture: FIFO level is outside native range");
    }
    resetPoint(pointFor(database, input, node.id), static_cast<std::int32_t>(level));
  }
  seedIdealClocks(database, input);
  seedStarts(database, input);
  seedArcs(database, input);
  propagateForward(database, input, fifo);
  propagateRequired(database, input, fifo);

  database.get_timing_order_list().clear();
  database.get_timing_order_list().reserve(fifo.level_nodes.size());
  for (NodeId node : fifo.level_nodes) {
    database.get_timing_order_list().push_back(input.nodes.at(node.value()).key.full_name);
  }
}

[[nodiscard]] CsrReferenceInput exportInput(const Database& database, const StaAnalysisContext& context, std::uint64_t token)
{
  CsrReferenceInputExportResult exported = exportCsrReferenceInputCandidate(database, context, token);
  if (const auto* failure = std::get_if<CsrReferenceInputExportFailure>(&exported)) {
    throw std::runtime_error("p/c " + std::to_string(static_cast<int>(failure->failed_phase)) + "/" + std::to_string(static_cast<int>(failure->code)));
  }
  return std::get<CsrReferenceInput>(std::move(exported));
}

[[nodiscard]] CsrTimingScatterTestFixture finishFixture(CsrReferenceInputNativeFixture native, std::uint64_t session_token)
{
  CsrTimingScatterTestFixture fixture;
  fixture.native = std::move(native);
  fixture.context = referenceInputTestContext();
  fixture.session_token = session_token;
  fixture.graph_generation = 0;
  fixture.committed_input = exportInput(fixture.database(), fixture.context, session_token);
  seedIndependentNativeTiming(fixture.database(), fixture.committed_input);
  fixture.expected_oracle = readNativeTimingOracle(fixture.database());
  fixture.forbidden_bytes = timingScatterForbiddenBytes(fixture.database(), fixture.context);
  return fixture;
}

void poisonPoint(TimingPoint& point, const std::string& reference_node, std::size_t reference_arc, double value)
{
  point.set_arrival(value);
  point.set_required(value + 1.0);
  point.set_slack(value + 2.0);
  point.set_launch_time(value + 3.0);
  point.set_level(static_cast<std::int32_t>(100 + static_cast<int>(value)));
  point.set_is_clock_point(!point.get_is_clock_point());
  point.set_clock_name("__scatter_poison_clock__");
  point.set_predecessor(reference_node);
  point.set_predecessor_arc_idx(reference_arc);
  point.get_clock_arrival_map() = {{AnalysisType::kMax, {{TransType::kRise, value + 4.0}}}};
  point.get_clock_slew_map() = {{AnalysisType::kMax, {{TransType::kRise, value + 5.0}}}};
  point.get_clock_predecessor_map() = {{AnalysisType::kMax, {{TransType::kRise, reference_node}}}};
  point.get_clock_predecessor_arc_delay_map() = {{AnalysisType::kMax, {{TransType::kRise, value + 6.0}}}};
  point.get_clock_predecessor_trans_type_map() = {{AnalysisType::kMax, {{TransType::kRise, TransType::kFall}}}};
  point.get_physical_clock_arrival_map() = {{AnalysisType::kMin, {{TransType::kFall, value + 7.0}}}};
  point.get_physical_clock_slew_map() = {{AnalysisType::kMin, {{TransType::kFall, value + 8.0}}}};
  point.get_physical_clock_predecessor_map() = {{AnalysisType::kMin, {{TransType::kFall, reference_node}}}};
  point.get_physical_clock_predecessor_arc_delay_map() = {{AnalysisType::kMin, {{TransType::kFall, value + 9.0}}}};
  point.get_physical_clock_predecessor_trans_type_map() = {{AnalysisType::kMin, {{TransType::kFall, TransType::kRise}}}};
  point.get_data_slew_map() = {{AnalysisType::kMax, {{TransType::kFall, value + 10.0}}}};
  TimingPathState path;
  path.set_arrival(value + 11.0);
  path.set_slew(value + 12.0);
  path.set_launch_time(value + 13.0);
  path.set_start_point(reference_node);
  path.set_clock_name("__scatter_poison_tag__");
  path.set_trans_type(TransType::kRise);
  point.get_path_state_map() = {{AnalysisType::kMax, {{PathSourceType::kInput, {{TransType::kRise, {{"__scatter_poison_tag__", std::move(path)}}}}}}}};
}

void poisonArc(Arc& arc, double value)
{
  arc.set_delay(value);
  arc.set_delay_min(value + 1.0);
  arc.set_delay_max(value + 2.0);
  arc.get_trans_delay_map() = {{AnalysisType::kMax, {{TransType::kRise, value + 3.0}}}};
  arc.get_input_output_delay_map() = {{AnalysisType::kMin, {{TransType::kFall, {{TransType::kRise, value + 4.0}}}}}};
  arc.get_graph_delay_map() = {{AnalysisType::kMax, {{TransType::kRise, {{TransType::kFall, value + 5.0}}}}}};
  arc.get_trans_type_map() = {{TransType::kRise, TransType::kFall}};
  TimingArcDelayState state;
  if (const TimingCellArc* cell_arc = std::as_const(arc).get_timing_cell_arc(); cell_arc != nullptr) {
    for (const TimingArc& child : cell_arc->get_timing_arc_list()) {
      const TimingArcDelayFallbackKey key{child.get_arc_idx(), AnalysisType::kMax, TransType::kRise, TransType::kRise};
      state.delay_map[child.get_arc_idx()][AnalysisType::kMax][TransType::kRise][TransType::kRise] = value + 6.0;
      state.fallback_key_set.insert(key);
    }
  }
  arc.replace_timing_arc_delay_state(std::move(state));
}

}  // namespace

Database& CsrTimingScatterTestFixture::database()
{
  return native.database();
}

const Database& CsrTimingScatterTestFixture::database() const
{
  return native.database();
}

CsrTimingScatterTestFixture makeTimingScatterFixture(ReferenceNativeFixtureCase fixture_case, std::uint64_t session_token)
{
  if (session_token == 0) {
    throw std::invalid_argument("timing scatter fixture: session token must be nonzero");
  }
  CsrReferenceInputNativeFixture native = makeReferenceNativeFixture(fixture_case);
  switch (fixture_case) {
    case ReferenceNativeFixtureCase::kChain:
      makeChainSupported(native.database());
      break;
    case ReferenceNativeFixtureCase::kDiamond: {
      native.database().get_timing_constraint().get_clock_map().begin()->second.set_source_list({"sink"});
      TimingPortConstraint input;
      input.set_port_name("root");
      input.set_input_delay_max(0.25);
      input.set_has_input_delay_max(true);
      native.database().get_timing_constraint().get_port_constraint_map().emplace("root", std::move(input));
    } break;
    default:
      break;
  }
  return finishFixture(std::move(native), session_token);
}

CsrTimingScatterTestFixture makeTimingScatterPropertyFixture(std::uint64_t seed)
{
  const std::uint64_t graph_seed = seed & ~std::uint64_t{1};
  std::uint64_t token = seed ^ 0x6a09e667f3bcc909ULL;
  if (token == 0) {
    token = 1;
  }
  return finishFixture(makeReferencePropertyFixture(graph_seed), token);
}

void poisonTimingScatterFixture(CsrTimingScatterTestFixture& fixture, std::uint64_t repeat_index)
{
  if (fixture.database().get_timing_point_map().empty() || fixture.database().get_arc_list().empty()) {
    throw std::invalid_argument("timing scatter poison requires nonempty node and Arc inventories");
  }
  const std::string reference_node = fixture.database().get_timing_point_map().begin()->first;
  for (auto& [name, point] : fixture.database().get_timing_point_map()) {
    static_cast<void>(name);
    poisonPoint(point, reference_node, 0, 1000.0 + static_cast<double>(repeat_index++));
  }
  for (Arc& arc : fixture.database().get_arc_list()) {
    poisonArc(arc, 2000.0 + static_cast<double>(repeat_index++));
  }
  std::ranges::reverse(fixture.database().get_timing_order_list());
}

void reverseTimingScatterNativeArcStorage(CsrTimingScatterTestFixture& fixture)
{
  std::vector<Arc> relocated = fixture.database().get_arc_list();
  fixture.database().get_arc_list().swap(relocated);
  fixture.forbidden_bytes = timingScatterForbiddenBytes(fixture.database(), fixture.context);
}

std::vector<std::uint8_t> timingScatterForbiddenBytes(const Database& database)
{
  return timingScatterForbiddenBytes(database, referenceInputTestContext());
}

std::vector<std::uint8_t> timingScatterForbiddenBytes(const Database& database, const StaAnalysisContext& context)
{
  constexpr std::uint64_t kEncodingToken = 0x6f726269744d6170ULL;
  return canonicalReferenceInputBytes(exportInput(database, context, kEncodingToken));
}

void requireTimingScatter(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireScatterCode(const CsrTimingResultScatterAttempt& attempt, CsrScatterCode code, std::uint64_t writes_applied,
                        std::optional<CsrInvalidationReason> invalidation_reason, const std::string& name)
{
  requireTimingScatter(attempt.scatter_result.code == code,
                       name + " returned scatter code " + std::to_string(static_cast<unsigned>(attempt.scatter_result.code)));
  requireTimingScatter(attempt.scatter_result.writes_applied == writes_applied,
                       name + " returned write count " + std::to_string(attempt.scatter_result.writes_applied));
  requireTimingScatter(attempt.scatter_result.invalidation_reason == invalidation_reason, name + " returned the wrong invalidation reason");
}

void requireNativeTimingOracleEqual(const CsrNativeTimingOracle& actual, const CsrNativeTimingOracle& expected, const std::string& name)
{
  requireTimingScatter(actual == expected, name + " native timing DTO differs");
  requireTimingScatter(canonicalIndependentNativeTimingOracleBytes(actual) == canonicalIndependentNativeTimingOracleBytes(expected),
                       name + " independent native timing bytes differ");
}

}  // namespace ista::csr::test
