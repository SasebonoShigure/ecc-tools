// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "CsrSnapshotStorage.hpp"

#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace ista::csr {
namespace {

std::vector<CsrContractViolation> overflow(std::string_view field)
{
  return {{std::string(field), "modeled storage byte count overflow"}};
}

template <typename T>
CsrCheckedStorageValue checkedSize(T value, std::string_view field)
{
  static_assert(std::is_unsigned_v<T>);
  if constexpr (sizeof(T) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max())
      return overflow(field);
  }
  return static_cast<std::uint64_t>(value);
}

class StorageWalker
{
 public:
  bool addRoot(std::uint64_t bytes, std::string_view field) { return addTo(_logical, bytes, field) && addTo(_capacity, bytes, field); }

  void visit(const std::string& value, std::string_view field)
  {
    addSize(_logical, value.size(), field);
    addSize(_capacity, value.capacity(), field);
  }

  template <typename T>
  void visitVector(const std::vector<T>& values, std::string_view field)
  {
    addProduct(_logical, values.size(), sizeof(T), field);
    addProduct(_capacity, values.capacity(), sizeof(T), field);
    for (const auto& value : values)
      visit(value, field);
  }

  template <typename T>
  void visitScalarVector(const std::vector<T>& values, std::string_view field)
  {
    addProduct(_logical, values.size(), sizeof(T), field);
    addProduct(_capacity, values.capacity(), sizeof(T), field);
  }

  [[nodiscard]] bool ok() const { return _errors.empty(); }
  [[nodiscard]] CsrModeledHostBytes bytes() const { return {_logical, _capacity}; }
  [[nodiscard]] std::vector<CsrContractViolation> takeErrors() { return std::move(_errors); }

 private:
  template <typename T>
  void addSize(std::uint64_t& total, T value, std::string_view field)
  {
    auto converted = checkedSize(value, field);
    if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&converted)) {
      _errors.insert(_errors.end(), errors->begin(), errors->end());
      return;
    }
    addTo(total, std::get<std::uint64_t>(converted), field);
  }

  template <typename T>
  void addProduct(std::uint64_t& total, T count, std::size_t width, std::string_view field)
  {
    auto converted_count = checkedSize(count, field);
    auto converted_width = checkedSize(width, field);
    if (std::holds_alternative<std::vector<CsrContractViolation>>(converted_count)
        || std::holds_alternative<std::vector<CsrContractViolation>>(converted_width)) {
      _errors.push_back(overflow(field).front());
      return;
    }
    auto product = checkedStorageMultiply(std::get<std::uint64_t>(converted_count), std::get<std::uint64_t>(converted_width), field);
    if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&product)) {
      _errors.insert(_errors.end(), errors->begin(), errors->end());
      return;
    }
    addTo(total, std::get<std::uint64_t>(product), field);
  }

  bool addTo(std::uint64_t& total, std::uint64_t value, std::string_view field)
  {
    auto sum = checkedStorageAdd(total, value, field);
    if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&sum)) {
      _errors.insert(_errors.end(), errors->begin(), errors->end());
      return false;
    }
    total = std::get<std::uint64_t>(sum);
    return true;
  }

  void visit(const NodeKey& value, std::string_view field)
  {
    visit(value.full_name, field);
    visit(value.instance_name, field);
    visit(value.pin_name, field);
  }

  void visit(const LibraryArcSignature& value, std::string_view field)
  {
    visit(value.library_name, field);
    visit(value.sdf_condition, field);
  }

  void visit(const ArcKeyBase& value, std::string_view field)
  {
    visit(value.source, field);
    visit(value.sink, field);
    visit(value.owner_name, field);
    visit(value.library_source_port, field);
    visit(value.library_sink_port, field);
    visitVector(value.library_arcs, field);
  }

  void visit(const ArcKey& value, std::string_view field) { visit(value.base, field); }
  void visit(const NodeIdLookupEntry& value, std::string_view field) { visit(value.key, field); }
  void visit(const ArcIdLookupEntry& value, std::string_view field) { visit(value.key, field); }

  void visit(const TimingTableModel& value, std::string_view field)
  {
    addProduct(_logical, value.axes.size(), sizeof(std::vector<double>), field);
    addProduct(_capacity, value.axes.capacity(), sizeof(std::vector<double>), field);
    for (const auto& axis : value.axes)
      visitScalarVector(axis, field);
    visitScalarVector(value.values, field);
  }

  void visit(const ChildTimingArcModel& value, std::string_view field)
  {
    visit(value.signature, field);
    for (const auto& table : value.delay_tables)
      visit(table, field);
    for (const auto& table : value.slew_tables)
      visit(table, field);
    for (const auto& table : value.check_tables)
      visit(table, field);
  }

  void visit(const CellTransferModel& value, std::string_view field) { visitVector(value.children, field); }

  void visit(const NodeRecord& value, std::string_view field)
  {
    visit(value.key, field);
    visit(value.instance_cell, field);
    visit(value.net_name, field);
  }

  void visit(const InstanceRecord& value, std::string_view field)
  {
    visit(value.instance_name, field);
    visit(value.cell_name, field);
    visitScalarVector(value.ordered_pins, field);
  }

  void visit(const ArcRecord& value, std::string_view field)
  {
    visit(value.key, field);
    visit(value.transfer, field);
  }

  void visit(const RawNodeReference& value, std::string_view field) { visit(value.full_name, field); }

  void visit(const ClockConstraintModel& value, std::string_view field)
  {
    visit(value.map_key, field);
    visit(value.embedded_clock_name, field);
    visitVector(value.ordered_sources, field);
  }

  void visit(const PortConstraintModel& value, std::string_view field)
  {
    visit(value.map_key, field);
    visit(value.embedded_port_name, field);
    visit(value.clock_name, field);
  }

  void visit(const CaseAnalysisConstraintModel& value, std::string_view field) { visit(value.raw_name, field); }

  void visit(const StartSeedModel& value, std::string_view field) { visit(value.clock_tag, field); }
  void visit(const EndpointRequiredModel& value, std::string_view field) { visit(value.clock_name, field); }
  void visit(const CellPinCapacitanceModel&, std::string_view) {}

  void visit(const TimingCheckEndpointModel& value, std::string_view field)
  {
    visit(value.instance_name, field);
    visit(value.clock_pin_raw_name, field);
    visit(value.data_pin_raw_name, field);
    visit(value.clock_pin_full_name, field);
    visit(value.data_pin_full_name, field);
  }

  void visit(const LibraryPortModel& value, std::string_view field) { visit(value.name, field); }

  void visit(const LibraryCellArcModel& value, std::string_view field)
  {
    visit(value.source_port, field);
    visit(value.sink_port, field);
    visit(value.transfer, field);
  }

  void visit(const LibraryTimingCheckArcModel& value, std::string_view field)
  {
    visit(value.clock_port, field);
    visit(value.data_port, field);
    visitVector(value.children, field);
  }

  void visit(const ReferencedLibraryCellModel& value, std::string_view field)
  {
    visit(value.cell_name, field);
    visit(value.library_name, field);
    visit(value.clock_pin_name, field);
    visit(value.data_pin_name, field);
    visit(value.output_pin_name, field);
    visitVector(value.ports, field);
    visitVector(value.cell_arcs, field);
    visitVector(value.timing_checks, field);
  }

  void visit(const ParasiticNodeModel& value, std::string_view field)
  {
    visit(value.map_key, field);
    visit(value.embedded_name, field);
  }

  void visit(const ParasiticResistorModel& value, std::string_view field)
  {
    visit(value.source, field);
    visit(value.sink, field);
  }

  void visit(const ParasiticNetModel& value, std::string_view field)
  {
    visit(value.map_key, field);
    visit(value.embedded_name, field);
    visitVector(value.nodes, field);
    visitVector(value.resistors, field);
  }

  void visit(const std::optional<std::string>& value, std::string_view field)
  {
    if (value)
      visit(*value, field);
  }

  void visit(CsrReferenceUnsupportedReason, std::string_view) {}
  void visit(NodeId, std::string_view) {}
  void visit(ArcId, std::string_view) {}
  void visit(std::uint64_t, std::string_view) {}

  std::uint64_t _logical{0};
  std::uint64_t _capacity{0};
  std::vector<CsrContractViolation> _errors;
};

std::variant<CsrBufferExtent, std::vector<CsrContractViolation>> extent(std::size_t logical, std::size_t capacity, std::string_view field)
{
  auto logical_value = checkedSize(logical, field);
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&logical_value))
    return *errors;
  auto capacity_value = checkedSize(capacity, field);
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&capacity_value))
    return *errors;
  return CsrBufferExtent{std::get<std::uint64_t>(logical_value), std::get<std::uint64_t>(capacity_value)};
}

bool setExtent(CsrBufferExtent& target, std::size_t logical, std::size_t capacity, std::string_view field, std::vector<CsrContractViolation>& errors)
{
  auto value = extent(logical, capacity, field);
  if (const auto* found = std::get_if<std::vector<CsrContractViolation>>(&value)) {
    errors.insert(errors.end(), found->begin(), found->end());
    return false;
  }
  target = std::get<CsrBufferExtent>(value);
  return true;
}

void visitRootStrings(StorageWalker& walker, const CsrReferenceInput& input)
{
  walker.visit(input.schema_version, "storage.input.schema_version");
  walker.visit(input.source_identity.fingerprint_schema_version, "storage.input.identity.fingerprint_schema_version");
  walker.visit(input.source_identity.topology_fingerprint, "storage.input.identity.topology_fingerprint");
  walker.visit(input.source_identity.clock_constraint_fingerprint, "storage.input.identity.clock_constraint_fingerprint");
  walker.visit(input.source_identity.library_fingerprint, "storage.input.identity.library_fingerprint");
  walker.visit(input.source_identity.corner_fingerprint, "storage.input.identity.corner_fingerprint");
  walker.visit(input.source_identity.parasitic_fingerprint, "storage.input.identity.parasitic_fingerprint");
  walker.visit(input.source_identity.timing_input_fingerprint, "storage.input.identity.timing_input_fingerprint");
  walker.visit(input.analysis_context.liberty_corner, "storage.input.analysis_context.liberty_corner");
  walker.visit(input.analysis_context.temperature, "storage.input.analysis_context.temperature");
  walker.visit(input.analysis_context.rc_corner, "storage.input.analysis_context.rc_corner");
  walker.visit(input.parasitics.capacitance_unit, "storage.input.parasitics.capacitance_unit");
  walker.visit(input.parasitics.resistance_unit, "storage.input.parasitics.resistance_unit");
}

void visitInputVectors(StorageWalker& walker, const CsrReferenceInput& input)
{
  walker.visitVector(input.nodes, "storage.input.nodes");
  walker.visitVector(input.node_ids_by_key, "storage.input.node_ids_by_key");
  walker.visitVector(input.instances, "storage.input.instances");
  walker.visitVector(input.arcs, "storage.input.arcs");
  walker.visitVector(input.arc_ids_by_key, "storage.input.arc_ids_by_key");
  walker.visitScalarVector(input.out_row_offsets, "storage.input.out_row_offsets");
  walker.visitScalarVector(input.out_arc_ids, "storage.input.out_arc_ids");
  walker.visitScalarVector(input.in_row_offsets, "storage.input.in_row_offsets");
  walker.visitScalarVector(input.in_arc_ids, "storage.input.in_arc_ids");
  walker.visitScalarVector(input.ordered_root_ids, "storage.input.ordered_root_ids");
  walker.visitScalarVector(input.ordered_start_ids, "storage.input.ordered_start_ids");
  walker.visitScalarVector(input.ordered_end_ids, "storage.input.ordered_end_ids");
  walker.visitVector(input.clock_tags, "storage.input.clock_tags");
  walker.visitVector(input.clocks, "storage.input.clocks");
  walker.visitVector(input.clock_owner_by_node, "storage.input.clock_owner_by_node");
  walker.visitVector(input.port_constraints, "storage.input.port_constraints");
  walker.visitVector(input.case_analysis_constraints, "storage.input.case_analysis_constraints");
  walker.visitVector(input.timing_check_endpoints, "storage.input.timing_check_endpoints");
  walker.visitVector(input.referenced_library_cells, "storage.input.referenced_library_cells");
  walker.visitVector(input.parasitics.nets, "storage.input.parasitics.nets");
  walker.visitVector(input.start_seeds, "storage.input.start_seeds");
  walker.visitVector(input.endpoint_required, "storage.input.endpoint_required");
  walker.visitVector(input.cell_pin_capacitances, "storage.input.cell_pin_capacitances");
  walker.visitVector(input.unsupported_reasons, "storage.input.unsupported_reasons");
}

}  // namespace

CsrCheckedStorageValue checkedStorageAdd(std::uint64_t lhs, std::uint64_t rhs, std::string_view field)
{
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    return overflow(field);
  return lhs + rhs;
}

CsrCheckedStorageValue checkedStorageMultiply(std::uint64_t lhs, std::uint64_t rhs, std::string_view field)
{
  if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
    return overflow(field);
  return lhs * rhs;
}

CsrCheckedStorageValue checkedStorageSum(std::span<const std::uint64_t> values, std::string_view field)
{
  std::uint64_t result = 0;
  for (std::uint64_t value : values) {
    auto sum = checkedStorageAdd(result, value, field);
    if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&sum))
      return *errors;
    result = std::get<std::uint64_t>(sum);
  }
  return result;
}

CsrSnapshotStorageResult snapshotStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata)
{
  CsrSnapshotStorageReport report;
  std::vector<CsrContractViolation> errors;
  setExtent(report.buffers.node_keys, input.nodes.size(), input.nodes.capacity(), "storage.buffers.node_keys", errors);
  setExtent(report.buffers.arc_keys, input.arcs.size(), input.arcs.capacity(), "storage.buffers.arc_keys", errors);
  setExtent(report.buffers.out_row_offsets, input.out_row_offsets.size(), input.out_row_offsets.capacity(), "storage.buffers.out_row_offsets", errors);
  setExtent(report.buffers.out_arc_ids, input.out_arc_ids.size(), input.out_arc_ids.capacity(), "storage.buffers.out_arc_ids", errors);
  setExtent(report.buffers.in_row_offsets, input.in_row_offsets.size(), input.in_row_offsets.capacity(), "storage.buffers.in_row_offsets", errors);
  setExtent(report.buffers.in_arc_ids, input.in_arc_ids.size(), input.in_arc_ids.capacity(), "storage.buffers.in_arc_ids", errors);
  setExtent(report.buffers.level_offsets, metadata.level_offsets.size(), metadata.level_offsets.capacity(), "storage.buffers.level_offsets", errors);
  setExtent(report.buffers.level_nodes, metadata.level_nodes.size(), metadata.level_nodes.capacity(), "storage.buffers.level_nodes", errors);
  setExtent(report.buffers.node_to_level, metadata.node_to_level.size(), metadata.node_to_level.capacity(), "storage.buffers.node_to_level", errors);
  if (!errors.empty())
    return errors;

  StorageWalker walker;
  walker.addRoot(sizeof(CsrReferenceInput), "storage.root.input");
  walker.addRoot(sizeof(CsrCaptureMetadata), "storage.root.metadata");
  visitRootStrings(walker, input);
  visitInputVectors(walker, input);
  walker.visitScalarVector(metadata.level_offsets, "storage.metadata.level_offsets");
  walker.visitScalarVector(metadata.level_nodes, "storage.metadata.level_nodes");
  walker.visitScalarVector(metadata.node_to_level, "storage.metadata.node_to_level");
  if (!walker.ok())
    return walker.takeErrors();
  report.host_bytes = walker.bytes();
  return report;
}

}  // namespace ista::csr
