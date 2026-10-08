// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CanonicalTimingRecordProjectorInternal.hpp"

#include <cstddef>
#include <set>
#include <stdexcept>

#include "CsrReferenceInputAdapterInternal.hpp"
#include "TimingGraphCsrAdapterInternal.hpp"

namespace ista::csr::detail {
namespace {

std::size_t analysisIndex(ista::AnalysisType analysis)
{
  switch (analysis) {
    case ista::AnalysisType::kMax:
      return 0;
    case ista::AnalysisType::kMin:
      return 1;
    case ista::AnalysisType::kNone:
      malformedReferenceInput();
  }
  malformedReferenceInput();
}

std::size_t transitionIndex(ista::TransType transition)
{
  switch (transition) {
    case ista::TransType::kRise:
      return 0;
    case ista::TransType::kFall:
      return 1;
    case ista::TransType::kNone:
      invalidReferenceTable();
  }
  invalidReferenceTable();
}

LibraryTimingCheckArcModel projectLibraryTimingCheck(
    const ista::TimingCheckArc& native_check)
{
  LibraryTimingCheckArcModel check;
  check.clock_port = native_check.get_clock_port();
  check.data_port = native_check.get_data_port();
  check.kind = convertReferenceTimingCheckKind(
      native_check.get_check_type());
  check.check_time = native_check.get_check_time();
  check.clock_transition = convertReferenceTransition(
      native_check.get_clock_trans_type());
  requireFiniteReference(check.check_time);
  if (check.clock_transition == Transition::kNone)
    malformedReferenceInput();
  std::vector<ChildTimingArcModel> children;
  children.reserve(native_check.get_timing_arc_list().size());
  for (const auto& native_child : native_check.get_timing_arc_list())
    children.push_back(exportReferenceChildTimingArc(native_child));
  check.setChildren(children);
  return check;
}

void assignLibraryRoles(const ista::TimingCell& native_cell,
                        ReferencedLibraryCellModel& cell)
{
  const ista::TimingCellArc* selected_clock_arc = nullptr;
  for (const auto& native_arc : native_cell.get_cell_arc_list()) {
    if (native_arc.get_is_clock_arc()) {
      selected_clock_arc = &native_arc;
      break;
    }
  }
  if (selected_clock_arc != nullptr)
    cell.output_pin_name = selected_clock_arc->get_sink_port();

  const ista::TimingCheckArc* representative_check = nullptr;
  for (const auto& native_check : native_cell.get_check_arc_list()) {
    if (representative_check == nullptr
        || native_check.get_check_type() == ista::TimingCheckType::kSetup)
      representative_check = &native_check;
    if (native_check.get_check_type() == ista::TimingCheckType::kSetup)
      break;
  }
  if (representative_check != nullptr) {
    cell.clock_pin_name = representative_check->get_clock_port();
    cell.data_pin_name = representative_check->get_data_port();
  }

  if (cell.output_pin_name.empty()) {
    for (const auto& [map_key, native_port] :
         native_cell.get_port_map()) {
      static_cast<void>(map_key);
      if (native_port.get_is_output() && !native_port.get_is_clock()) {
        cell.output_pin_name = native_port.get_port_name();
        break;
      }
    }
  }
}

bool isOutputLike(PinDirection direction)
{
  return direction == PinDirection::kOutput
         || direction == PinDirection::kInout;
}

void addCheckedLoad(double value, double& total)
{
  requireNonnegativeReference(value);
  total += value;
  requireNonnegativeReference(total);
}

}  // namespace

void NativeLibraryModels::requireDatabase(const ista::Database& database) const
{
  if (&database != &_database)
    throw std::invalid_argument("Liberty model store belongs to another Database");
}
const ReferencedLibraryCellModel& NativeLibraryModels::cell(const std::string& name)
{
  if (const auto found = _cells.find(name); found != _cells.end())
    return found->second;
  auto model = projectCanonicalReferencedLibraryCell(_database, name);
  std::map<const ista::TimingCellArc*, NativeArcSlot> arcs;
  const auto native = _database.get_timing_library().get_cell_map().find(name);
  if (native != _database.get_timing_library().get_cell_map().end()) {
    std::size_t ordinal = 0;
    for (const auto& arc : native->second.get_cell_arc_list())
      arcs.emplace(&arc, NativeArcSlot{nullptr, ordinal++});
  }
  auto result = _cells.emplace(name, std::move(model)).first;
  for (auto& [native_arc, slot] : arcs) {
    static_cast<void>(native_arc);
    slot.owner = &result->second;
  }
  _arcs.merge(arcs);
  return result->second;
}
std::uint64_t NativeLibraryModels::arcOrdinal(const std::string& name, const ista::TimingCellArc& arc)
{
  const auto& owner = cell(name);
  const auto found = _arcs.find(&arc);
  if (found == _arcs.end() || found->second.owner != &owner)
    malformedReferenceInput();
  return found->second.ordinal;
}

LibraryPortModel projectCanonicalLibraryPort(
    const ista::TimingCellPort& native_port)
{
  LibraryPortModel port;
  port.name = native_port.get_port_name();
  port.direction = convertReferencePortDirection(
      native_port.get_is_input(), native_port.get_is_output());
  port.is_clock = native_port.get_is_clock();
  port.is_output = native_port.get_is_output();
  port.default_capacitance = native_port.get_capacitance();
  requireNonnegativeReference(port.default_capacitance);

  for (const auto& [analysis, transitions] :
       native_port.get_trans_capacitance_map()) {
    const std::size_t analysis_index = analysisIndex(analysis);
    for (const auto& [transition, value] : transitions) {
      const std::size_t transition_index = transitionIndex(transition);
      requireNonnegativeReference(value);
      port.transition_capacitance[analysis_index][transition_index] = value;
    }
  }
  return port;
}

ReferencedLibraryCellModel projectCanonicalReferencedLibraryCell(
    const ista::Database& database, const std::string& cell_name)
{
  ReferencedLibraryCellModel cell;
  cell.cell_name = cell_name;

  const auto& cells = database.get_timing_library().get_cell_map();
  const auto found = cells.find(cell_name);
  if (found == cells.end())
    return cell;
  const ista::TimingCell& native_cell = found->second;
  if (native_cell.get_cell_name() != cell_name)
    malformedReferenceInput();

  cell.present = true;
  cell.library_name = native_cell.get_library_name();
  if (cell.library_name.empty())
    malformedReferenceInput();
  cell.is_sequential = native_cell.get_is_sequential();
  cell.is_clock_gating = native_cell.get_is_clock_gating();
  cell.is_macro = native_cell.get_is_macro();
  cell.slew_derate = native_cell.get_slew_derate_from_library();
  requireFiniteReference(cell.slew_derate);
  cell.slew_lower_threshold_pct = {native_cell.get_slew_lower_threshold_pct_rise(), native_cell.get_slew_lower_threshold_pct_fall()};
  cell.slew_upper_threshold_pct = {native_cell.get_slew_upper_threshold_pct_rise(), native_cell.get_slew_upper_threshold_pct_fall()};
  cell.input_threshold_pct = {native_cell.get_input_threshold_pct_rise(), native_cell.get_input_threshold_pct_fall()};
  for (auto* thresholds : {&cell.slew_lower_threshold_pct, &cell.slew_upper_threshold_pct, &cell.input_threshold_pct})
    for (auto& value : *thresholds) {
      requireFiniteReference(value);
      if (value > 1.0) value *= 0.01;
      if (value < 0.0 || value > 1.0) illegalReferenceNumeric();
    }
  assignLibraryRoles(native_cell, cell);

  cell.ports.reserve(native_cell.get_port_map().size());
  for (const auto& [map_key, native_port] :
       native_cell.get_port_map()) {
    if (map_key != native_port.get_port_name())
      malformedReferenceInput();
    cell.ports.push_back(projectCanonicalLibraryPort(native_port));
  }

  cell.cell_arcs.reserve(native_cell.get_cell_arc_list().size());
  for (const auto& native_arc : native_cell.get_cell_arc_list()) {
    if (native_arc.get_source_port().empty()
        || native_arc.get_sink_port().empty()
        || native_cell.get_port_map().find(
               native_arc.get_source_port())
               == native_cell.get_port_map().end()
        || native_cell.get_port_map().find(
               native_arc.get_sink_port())
               == native_cell.get_port_map().end())
      malformedReferenceInput();
    cell.cell_arcs.push_back(
        {native_arc.get_source_port(),
         native_arc.get_sink_port(),
         exportReferenceCellTransfer(native_arc)});
  }

  cell.timing_checks.reserve(native_cell.get_check_arc_list().size());
  for (const auto& native_check : native_cell.get_check_arc_list())
    cell.timing_checks.push_back(
        projectLibraryTimingCheck(native_check));
  return cell;
}

CellPinCapacitanceModel projectCanonicalCellPinCapacitance(
    const ista::Database& database, NodeId node, const NodeKey& key)
{
  if (key.kind != NodeKind::kInstancePin || key.full_name.empty()
      || key.instance_name.empty() || key.pin_name.empty())
    malformedReferenceInput();
  const auto native_pin = database.get_pin_map().find(key.full_name);
  const auto native_instance
      = database.get_instance_map().find(key.instance_name);
  if (native_pin == database.get_pin_map().end()
      || native_instance == database.get_instance_map().end()
      || native_pin->second.get_is_port()
      || native_pin->second.get_instance_name() != key.instance_name
      || native_pin->second.get_pin_name() != key.pin_name
      || native_instance->second.get_instance_name() != key.instance_name)
    malformedReferenceInput();

  CellPinCapacitanceModel result;
  result.node = node;
  const auto& cells = database.get_timing_library().get_cell_map();
  const auto native_cell
      = cells.find(native_instance->second.get_cell_name());
  if (native_cell == cells.end())
    return result;
  if (native_cell->second.get_cell_name()
      != native_instance->second.get_cell_name())
    malformedReferenceInput();
  const auto native_port
      = native_cell->second.get_port_map().find(key.pin_name);
  if (native_port == native_cell->second.get_port_map().end())
    return result;
  const LibraryPortModel port
      = projectCanonicalLibraryPort(native_port->second);
  result.default_capacitance = port.default_capacitance;
  result.transition_capacitance = port.transition_capacitance;
  return result;
}

void applyCanonicalInstanceLibraryRoles(
    const ReferencedLibraryCellModel& cell, InstanceRecord& instance)
{
  if (cell.cell_name != instance.cell_name)
    malformedReferenceInput();
  instance.is_sequential = cell.present && cell.is_sequential;
  instance.is_clock_gating = cell.present && cell.is_clock_gating;
  instance.is_macro = cell.present && cell.is_macro;
}

void applyCanonicalNodeLibraryRoles(
    const ReferencedLibraryCellModel& cell, NodeRecord& node)
{
  if (node.is_port || node.instance_cell != cell.cell_name)
    malformedReferenceInput();
  node.is_sequential = cell.present && cell.is_sequential;
  node.is_clock_pin = cell.present
                      && node.key.pin_name == cell.clock_pin_name;
  node.is_data_pin = cell.present
                     && node.key.pin_name == cell.data_pin_name;
  node.is_output_pin = cell.present
                       && node.key.pin_name == cell.output_pin_name;
}

ArcOutputLoadMatrix projectCanonicalNoParasiticNetLoad(
    const ista::Database& database, const std::string& net_name,
    const std::map<std::string, CanonicalLoadPinFacts>& load_pin_facts)
{
  const auto native_net = database.get_net_map().find(net_name);
  if (native_net == database.get_net_map().end()
      || native_net->second.get_net_name() != net_name)
    malformedReferenceInput();

  ArcOutputLoadMatrix result{};
  std::set<std::string> seen_loads;
  for (const auto& load_pin_name : native_net->second.get_load_pin_list()) {
    if (!seen_loads.emplace(load_pin_name).second)
      malformedReferenceInput();
    const auto native_pin = database.get_pin_map().find(load_pin_name);
    const auto facts = load_pin_facts.find(load_pin_name);
    if (native_pin == database.get_pin_map().end()
        || facts == load_pin_facts.end()
        || native_pin->second.get_full_name() != load_pin_name
        || native_pin->second.get_net_name() != net_name
        || facts->second.key.full_name != load_pin_name
        || facts->second.is_port != native_pin->second.get_is_port()
        || facts->second.direction
               != convertPinDirection(native_pin->second.get_direction()))
      malformedReferenceInput();

    if (facts->second.is_port) {
      if (facts->second.cell_pin_capacitance
          || (!isOutputLike(facts->second.direction)
              && facts->second.port_output_load))
        malformedReferenceInput();
      if (!facts->second.port_output_load)
        continue;
      for (auto& analysis : result)
        for (double& transition : analysis)
          addCheckedLoad(*facts->second.port_output_load, transition);
      continue;
    }

    if (facts->second.port_output_load
        || !facts->second.cell_pin_capacitance)
      malformedReferenceInput();
    const auto& capacitance = *facts->second.cell_pin_capacitance;
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
      for (std::size_t transition = 0; transition < kTransitionCount;
           ++transition) {
        addCheckedLoad(
            capacitance.transition_capacitance[analysis][transition]
                .value_or(capacitance.default_capacitance),
            result[analysis][transition]);
      }
    }
  }
  return result;
}

}  // namespace ista::csr::detail
