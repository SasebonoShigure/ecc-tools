// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeBufferObservationExtractor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "CsrTableLookup.hpp"
#include "Database.hpp"
#include "NativeBufferCellValidation.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditResolver.hpp"

namespace ista::buffer_trace {
namespace {

[[noreturn]] void fail(NativeObservationFailureCode code, std::string message)
{
  throw NativeObservationError(code, std::move(message));
}

csr::ChildTimingArcModel exportBufferArc(const TimingArc& child)
{
  try {
    return csr::detail::exportReferenceChildTimingArc(child);
  } catch (const std::exception&) {
    fail(NativeObservationFailureCode::kInvalidLibraryTable,
         "native buffer Liberty arc table export failed");
  }
}

std::optional<double> lookupBufferDelay(
    const csr::ChildTimingArcModel& exported, std::size_t transition,
    const SelectionProfile& profile)
{
  try {
    return csr::lookupCsrTimingTable(
        exported.delay_tables[transition],
        profile.operating_point.input_slew_ns,
        profile.operating_point.output_load_pf, exported.time_unit_scale,
        exported.capacitance_unit_scale);
  } catch (const std::exception&) {
    fail(NativeObservationFailureCode::kInvalidLibraryTable,
         "native buffer Liberty arc table lookup failed");
  }
}

double requireCapacitance(const TimingCellPort& port)
{
  double result = port.get_capacitance();
  const auto analysis = port.get_trans_capacitance_map().find(AnalysisType::kMax);
  if (analysis != port.get_trans_capacitance_map().end()) {
    for (TransType transition : {TransType::kRise, TransType::kFall}) {
      const auto value = analysis->second.find(transition);
      if (value != analysis->second.end()) {
        result = std::max(result, value->second);
      }
    }
  }
  if (!std::isfinite(result) || result < 0.0) {
    fail(NativeObservationFailureCode::kInvalidNativeDatabase,
         "native Liberty pin capacitance is non-finite or negative");
  }
  return result;
}

std::optional<MaterializedSetupTiming> setupTiming(
    const Database& database, const std::string& pin_name)
{
  const auto point = database.get_timing_point_map().find(pin_name);
  if (point == database.get_timing_point_map().end()) {
    return std::nullopt;
  }
  const auto& path_states = point->second.get_path_state_map();
  const auto maximum = path_states.find(AnalysisType::kMax);
  if (maximum == path_states.end()) {
    return std::nullopt;
  }
  bool finite_setup_path = false;
  for (const auto& source_entry : maximum->second)
    for (const auto& transition_entry : source_entry.second)
      for (const auto& clock_entry : transition_entry.second)
        finite_setup_path = finite_setup_path
                            || std::isfinite(clock_entry.second.get_arrival());
  if (!finite_setup_path) {
    return std::nullopt;
  }
  const double arrival_ns = point->second.get_arrival();
  const double required_ns = point->second.get_required();
  const double slack_ns = point->second.get_slack();
  if (arrival_ns == 0.0 && required_ns == 0.0 && slack_ns == 0.0) {
    return std::nullopt;
  }
  return MaterializedSetupTiming{arrival_ns, required_ns, slack_ns};
}

std::optional<std::pair<const TimingCell&, const TimingCellPort&>> sinkCellPort(
    const Database& database, const Pin& sink)
{
  if (sink.get_is_port() || sink.get_direction() != PinDirection::kInput
      || sink.get_instance_name().empty()) {
    return std::nullopt;
  }
  const auto instance = database.get_instance_map().find(sink.get_instance_name());
  if (instance == database.get_instance_map().end()
      || instance->second.get_is_sequential()
      || instance->second.get_is_clock_gating()
      || instance->second.get_clock_pin_name() == sink.get_pin_name()) {
    return std::nullopt;
  }
  const auto cell = database.get_timing_library().get_cell_map().find(
      instance->second.get_cell_name());
  if (cell == database.get_timing_library().get_cell_map().end()) {
    return std::nullopt;
  }
  const auto port = cell->second.get_port_map().find(sink.get_pin_name());
  if (port == cell->second.get_port_map().end() || !port->second.get_is_input()
      || port->second.get_is_output() || port->second.get_is_clock()) {
    return std::nullopt;
  }
  return std::pair<const TimingCell&, const TimingCellPort&>{cell->second,
                                                            port->second};
}

struct LegalBuffer
{
  timing_edit::LibraryCellRef reference;
  double capacitance_pf{0.0};
  double delay_ns{0.0};
};

std::optional<LegalBuffer> legalBuffer(const Database& database,
                                       const TimingCell& cell,
                                       const SelectionProfile& profile)
{
  if (cell.get_is_sequential() || cell.get_is_clock_gating()
      || cell.get_is_macro() || cell.get_port_map().size() != 2) {
    return std::nullopt;
  }
  const TimingCellPort* input = nullptr;
  const TimingCellPort* output = nullptr;
  for (const auto& [name, port] : cell.get_port_map()) {
    static_cast<void>(name);
    if (port.get_is_input() && !port.get_is_output() && !port.get_is_clock()) {
      if (input != nullptr) {
        return std::nullopt;
      }
      input = &port;
    } else if (port.get_is_output() && !port.get_is_input()
               && !port.get_is_clock()) {
      if (output != nullptr) {
        return std::nullopt;
      }
      output = &port;
    } else {
      return std::nullopt;
    }
  }
  if (input == nullptr || output == nullptr) {
    return std::nullopt;
  }

  const TimingCellArc* selected_arc = nullptr;
  for (const TimingCellArc& arc : cell.get_cell_arc_list()) {
    if (!arc.get_is_timing_graph_arc() || arc.get_is_clock_arc()
        || arc.get_is_disable_arc() || arc.get_is_clear_preset_arc()) {
      continue;
    }
    if (selected_arc != nullptr || arc.get_source_port() != input->get_port_name()
        || arc.get_sink_port() != output->get_port_name()) {
      return std::nullopt;
    }
    selected_arc = &arc;
  }
  if (selected_arc == nullptr || selected_arc->get_timing_arc_list().empty()) {
    return std::nullopt;
  }

  std::vector<csr::LibraryArcSignature> signatures;
  std::array<std::optional<double>, csr::kTransitionCount> delays;
  for (const TimingArc& child : selected_arc->get_timing_arc_list()) {
    const csr::ChildTimingArcModel exported = exportBufferArc(child);
    if (exported.signature.sense != csr::TimingSense::kPositiveUnate) {
      return std::nullopt;
    }
    signatures.push_back(exported.signature);
    for (std::size_t transition = 0; transition < csr::kTransitionCount;
         ++transition) {
      const auto delay = lookupBufferDelay(exported, transition, profile);
      if (delay) {
        delays[transition]
            = delays[transition] ? std::max(*delays[transition], *delay) : *delay;
      }
    }
  }
  if (!delays[0] || !delays[1] || !std::isfinite(*delays[0])
      || !std::isfinite(*delays[1]) || *delays[0] < 0.0 || *delays[1] < 0.0) {
    return std::nullopt;
  }
  timing_edit::LibraryCellRef reference{cell.get_library_name(),
                                        cell.get_cell_name(),
                                        input->get_port_name(),
                                        output->get_port_name(),
                                        std::move(signatures)};
  try {
    static_cast<void>(timing_edit::requireLegalNativeBufferCell(database,
                                                                reference));
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
  return LegalBuffer{std::move(reference), requireCapacitance(*input),
                     std::max(*delays[0], *delays[1])};
}

std::vector<LegalBuffer> legalBuffers(const Database& database,
                                      const SelectionProfile& profile)
{
  std::vector<LegalBuffer> buffers;
  for (const auto& [name, cell] : database.get_timing_library().get_cell_map()) {
    static_cast<void>(name);
    if (auto buffer = legalBuffer(database, cell, profile)) {
      buffers.push_back(std::move(*buffer));
    }
  }
  return buffers;
}

}  // namespace

const char* token(NativeObservationFailureCode code) noexcept
{
  switch (code) {
    case NativeObservationFailureCode::kInvalidProfile:
      return "invalid_observation_profile";
    case NativeObservationFailureCode::kInvalidNativeDatabase:
      return "invalid_observation_database";
    case NativeObservationFailureCode::kInvalidResolver:
      return "invalid_observation_resolver";
    case NativeObservationFailureCode::kInvalidLibraryTable:
      return "invalid_observation_library_table";
  }
  return "unknown_native_observation_failure";
}

NativeObservationError::NativeObservationError(
    NativeObservationFailureCode code, std::string message)
    : std::runtime_error(std::string(token(code)) + ": " + std::move(message)),
      _code(code)
{
}

std::vector<BufferCandidateObservation>
NativeBufferObservationExtractor::extract(
    const Database& database,
    const timing_edit::TimingEditResolverState& resolver,
    const SelectionProfile& profile)
{
  if (const auto violations = validateSelectionProfile(profile);
      !violations.empty()) {
    fail(NativeObservationFailureCode::kInvalidProfile,
         violations.front().field + ": " + violations.front().message);
  }
  const std::vector<LegalBuffer> buffers = legalBuffers(database, profile);
  std::vector<BufferCandidateObservation> result;
  for (const auto& [net_name, net] : database.get_net_map()) {
    if (net.get_driver_pin_list().size() != 1 || net.get_load_pin_list().empty()) {
      continue;
    }
    const std::string& driver_name = net.get_driver_pin_list().front();
    timing_edit::StableNetRef stable_net;
    timing_edit::StableNodeRef stable_driver;
    try {
      stable_net = resolver.stableNetForNativeName(net_name);
      stable_driver = resolver.stableNodeForNativeName(driver_name);
    } catch (const std::invalid_argument& error) {
      fail(NativeObservationFailureCode::kInvalidResolver, error.what());
    }
    for (const std::string& sink_name : net.get_load_pin_list()) {
      const auto sink = database.get_pin_map().find(sink_name);
      if (sink == database.get_pin_map().end()) {
        fail(NativeObservationFailureCode::kInvalidNativeDatabase,
             "net load does not resolve to a native pin");
      }
      const auto sink_cell = sinkCellPort(database, sink->second);
      const auto point = database.get_timing_point_map().find(sink_name);
      if (!sink_cell || (point != database.get_timing_point_map().end()
                         && point->second.get_is_clock_point())) {
        continue;
      }
      timing_edit::StableNodeRef stable_sink;
      try {
        stable_sink = resolver.stableNodeForNativeName(sink_name);
      } catch (const std::invalid_argument& error) {
        fail(NativeObservationFailureCode::kInvalidResolver, error.what());
      }
      const double load_pf = requireCapacitance(sink_cell->second);
      for (const LegalBuffer& buffer : buffers) {
        result.push_back(BufferCandidateObservation{
            .net = stable_net,
            .driver = stable_driver,
            .candidate_pin = stable_sink,
            .moved_sinks = {stable_sink},
            .setup_timing = setupTiming(database, sink_name),
            .upstream_resistance_kohm
            = profile.rc_profile.upstream_resistance_kohm,
            .candidate_load_pf = load_pf,
            .buffer_capacitance_pf = buffer.capacitance_pf,
            .buffer_resistance_kohm
            = profile.rc_profile.buffer_resistance_kohm,
            .buffer_delay_ns = buffer.delay_ns,
            .buffer = buffer.reference,
            .placement = timing_edit::UnplacedInputs{},
            .rc = timing_edit::NoParasiticRcInputs{}});
      }
    }
  }
  return result;
}

}  // namespace ista::buffer_trace
