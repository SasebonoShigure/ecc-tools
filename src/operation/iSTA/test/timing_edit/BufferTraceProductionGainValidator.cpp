// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceProductionGainValidator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>

#include "CsrTableLookup.hpp"
#include "TimingEditResolver.hpp"

namespace ista::buffer_trace::test_support {
namespace {

[[noreturn]] void fail(const std::string& message)
{
  throw std::runtime_error("production gain validation: " + message);
}

const csr::NodeRecord& resolveNode(
    const csr::CsrReferenceInput& input,
    const timing_edit::TimingEditResolverState& resolver,
    const timing_edit::StableNodeRef& ref)
{
  const std::string& native_name = resolver.resolveNode(ref);
  const auto found = std::find_if(
      input.nodes.begin(), input.nodes.end(), [&](const csr::NodeRecord& node) {
        return node.key.full_name == native_name;
      });
  if (found == input.nodes.end())
    fail("accepted candidate pin is absent from the batch CSR input");
  return *found;
}

double maxCapacitance(const csr::CellPinCapacitanceModel& model)
{
  double result = model.default_capacitance;
  for (std::size_t transition = 0; transition < csr::kTransitionCount;
       ++transition) {
    if (model.transition_capacitance[0][transition])
      result = std::max(result,
                        *model.transition_capacitance[0][transition]);
  }
  if (!std::isfinite(result) || result < 0.0)
    fail("CSR pin capacitance is non-finite or negative");
  return result;
}

double candidateLoad(const csr::CsrReferenceInput& input,
                     const csr::NodeRecord& node)
{
  const auto found = std::find_if(
      input.cell_pin_capacitances.begin(),
      input.cell_pin_capacitances.end(),
      [&](const csr::CellPinCapacitanceModel& item) {
        return item.node == node.id;
      });
  if (found == input.cell_pin_capacitances.end())
    fail("accepted sink lacks an exported CSR cell-pin capacitance");
  return maxCapacitance(*found);
}

double portCapacitance(const csr::LibraryPortModel& port)
{
  double result = port.default_capacitance;
  for (std::size_t transition = 0; transition < csr::kTransitionCount;
       ++transition) {
    if (port.transition_capacitance[0][transition])
      result = std::max(result, *port.transition_capacitance[0][transition]);
  }
  if (!std::isfinite(result) || result < 0.0)
    fail("CSR buffer input capacitance is non-finite or negative");
  return result;
}

struct BufferElectricalClosure
{
  double capacitance_pf{0.0};
  double delay_ns{0.0};
};

BufferElectricalClosure bufferClosure(
    const csr::CsrReferenceInput& input,
    const timing_edit::LibraryCellRef& buffer,
    const SelectionProfile& profile)
{
  const auto cell = std::find_if(
      input.referenced_library_cells.begin(),
      input.referenced_library_cells.end(),
      [&](const csr::ReferencedLibraryCellModel& item) {
        return item.present && item.library_name == buffer.library_name
               && item.cell_name == buffer.cell_name;
      });
  if (cell == input.referenced_library_cells.end())
    fail("selected buffer cell is absent from CSR Liberty closure");
  const auto port = std::find_if(
      cell->ports.begin(), cell->ports.end(),
      [&](const csr::LibraryPortModel& item) {
        return item.name == buffer.input_port;
      });
  if (port == cell->ports.end())
    fail("selected buffer input port is absent from CSR Liberty closure");
  const auto arc = std::find_if(
      cell->cell_arcs.begin(), cell->cell_arcs.end(),
      [&](const csr::LibraryCellArcModel& item) {
        return item.source_port == buffer.input_port
               && item.sink_port == buffer.output_port;
      });
  if (arc == cell->cell_arcs.end())
    fail("selected buffer arc is absent from CSR Liberty closure");

  std::array<std::optional<double>, csr::kTransitionCount> delays;
  std::size_t matched_children = 0;
  for (const csr::ChildTimingArcModel& child : arc->transfer.children) {
    if (std::find(buffer.ordered_arc_signatures.begin(),
                  buffer.ordered_arc_signatures.end(), child.signature)
        == buffer.ordered_arc_signatures.end()) {
      continue;
    }
    ++matched_children;
    for (std::size_t transition = 0; transition < csr::kTransitionCount;
         ++transition) {
      const auto value = csr::lookupCsrTimingTable(
          child.delay_tables[transition],
          profile.operating_point.input_slew_ns,
          profile.operating_point.output_load_pf, child.time_unit_scale,
          child.capacitance_unit_scale);
      if (value)
        delays[transition]
            = delays[transition] ? std::max(*delays[transition], *value)
                                 : *value;
    }
  }
  if (matched_children != buffer.ordered_arc_signatures.size()
      || !delays[0] || !delays[1] || !std::isfinite(*delays[0])
      || !std::isfinite(*delays[1]) || *delays[0] < 0.0
      || *delays[1] < 0.0) {
    fail("selected buffer CSR delay tables are incomplete or invalid");
  }
  return {portCapacitance(*port), std::max(*delays[0], *delays[1])};
}

double requireExactNumeric(const GainComputationReceipt& value,
                         double candidate_load,
                         const BufferElectricalClosure& buffer,
                         const SelectionProfile& profile)
{
  if (value.upstream_resistance_kohm
          != profile.rc_profile.upstream_resistance_kohm
      || value.buffer_resistance_kohm
             != profile.rc_profile.buffer_resistance_kohm
      || value.candidate_load_pf != candidate_load
      || value.buffer_capacitance_pf != buffer.capacitance_pf
      || value.buffer_delay_ns != buffer.delay_ns) {
    fail("generation receipt operands differ from production CSR closure");
  }
  const double delta = candidate_load - buffer.capacitance_pf;
  const double benefit = value.upstream_resistance_kohm * delta;
  const double penalty
      = value.buffer_resistance_kohm * buffer.capacitance_pf;
  const double net_benefit = benefit - penalty;
  const double gain = net_benefit - buffer.delay_ns;
  if (value.required_ns - value.arrival_ns != value.slack_ns
      || value.delta_capacitance_pf != delta || value.benefit_ns != benefit
      || value.penalty_ns != penalty
      || value.benefit_minus_penalty_ns != net_benefit
      || value.gain_ns != gain || !std::isfinite(gain)) {
    fail("generation receipt does not reproduce staged binary64 gain");
  }
  return gain;
}

}  // namespace

void requireProductionGenerationGain(
    const BufferTraceGenerationResult& generated)
{
  const auto& rounds = generated.artifact.generation_only.rounds;
  const auto& batch_receipts = generated.functional_receipt.batches;
  const auto& profile = generated.artifact.generation_only.profile;
  std::size_t accepted_count = 0;
  std::set<std::string> real_data_in_or2_loads;
  for (std::size_t round_index = 0; round_index < rounds.size(); ++round_index) {
    const auto& round = rounds[round_index];
    if (!round.accepted_tokens.empty() && round_index >= batch_receipts.size())
      fail("accepted round has no functional batch receipt");
    const auto& input = round_index < batch_receipts.size()
                            ? batch_receipts[round_index].csr_input
                            : generated.functional_receipt.final_csr_input;
    if (!input.unsupported_reasons.empty())
      fail("generation round has unsupported CSR reasons");
    std::size_t numeric_count = 0;
    for (const CandidateEvaluationReceipt& evaluation : round.evaluations) {
      if (!evaluation.numeric)
        continue;
      ++numeric_count;
      if (evaluation.decision == CandidateDecision::kAccepted
          && !evaluation.accepted_creation_token)
        fail("accepted generation evidence lacks numeric receipt or token");
      const csr::NodeRecord& sink = resolveNode(
          input, generated.functional_receipt.resolver_state,
          evaluation.candidate_pin);
      const double load = candidateLoad(input, sink);
      const BufferElectricalClosure buffer
          = bufferClosure(input, evaluation.buffer, profile);
      const double gain
          = requireExactNumeric(*evaluation.numeric, load, buffer, profile);
      const bool strict_positive = gain > +0.0;
      if (evaluation.decision == CandidateDecision::kRejected) {
        if (evaluation.rejection_reason
                != CandidateRejectionReason::kNonPositiveGain
            || strict_positive) {
          fail("numeric rejected candidate lacks exact nonpositive decision");
        }
      } else if (!strict_positive) {
        fail("ranked generation candidate lacks strict-positive gain");
      }
      if (generated.artifact.replay.header.workload_kind
              == SemanticWorkloadKind::kRealDesign
          && sink.instance_cell == "OR2_X1") {
        const auto* net
            = std::get_if<timing_edit::ExistingNetRef>(&evaluation.net);
        if (net != nullptr && net->baseline_name == "data_in") {
          real_data_in_or2_loads.insert(sink.key.full_name);
        }
      }
      if (evaluation.decision != CandidateDecision::kAccepted)
        continue;
      ++accepted_count;
    }
    if (round.accepted_tokens.empty() && round_index + 1 == rounds.size()
        && generated.artifact.generation_only.stop_reason
               == GenerationStopReason::kEmptyAcceptedBatch
        && numeric_count == 0) {
      fail("terminal empty round lacks production numeric evaluations");
    }
  }
  if (accepted_count == 0)
    fail("production generation accepted no strict-positive candidates");
  if (generated.artifact.replay.header.workload_kind
          == SemanticWorkloadKind::kRealDesign
      && real_data_in_or2_loads.size() < 4) {
    fail("real fixture did not prove at least four distinct OR2 input loads "
         "on the same baseline data_in net");
  }
}

}  // namespace ista::buffer_trace::test_support
