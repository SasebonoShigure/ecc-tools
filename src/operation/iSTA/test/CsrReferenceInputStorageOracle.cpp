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
#include "CsrReferenceInputStorageOracle.hpp"

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "CsrSnapshotTestFixture.hpp"

namespace ista::csr::snapshot_test {
namespace {

struct Bytes
{
  std::uint64_t logical{0};
  std::uint64_t capacity{0};
};

std::uint64_t add(std::uint64_t lhs, std::uint64_t rhs)
{
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    throw std::overflow_error("independent reference-input storage add overflow");
  return lhs + rhs;
}

std::uint64_t multiply(std::uint64_t lhs, std::uint64_t rhs)
{
  if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
    throw std::overflow_error("independent reference-input storage multiply overflow");
  return lhs * rhs;
}

std::uint64_t toU64(std::size_t value)
{
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    if (value > std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("independent reference-input storage conversion overflow");
  return static_cast<std::uint64_t>(value);
}

Bytes combine(Bytes lhs, Bytes rhs)
{
  return {add(lhs.logical, rhs.logical), add(lhs.capacity, rhs.capacity)};
}

Bytes slots(std::size_t size, std::size_t capacity, std::size_t width)
{
  return {multiply(toU64(size), toU64(width)), multiply(toU64(capacity), toU64(width))};
}

Bytes ownedString(const std::string& value)
{
  return {toU64(value.size()), toU64(value.capacity())};
}

template <typename T, typename Dynamic>
Bytes vectorOwned(const std::vector<T>& values, Dynamic dynamic)
{
  Bytes result = slots(values.size(), values.capacity(), sizeof(T));
  for (const auto& value : values)
    result = combine(result, dynamic(value));
  return result;
}

template <typename T>
Bytes scalarVectorOwned(const std::vector<T>& values)
{
  return slots(values.size(), values.capacity(), sizeof(T));
}

Bytes ownedTable(const TimingTableModel& value)
{
  Bytes result = vectorOwned(value.axes, [](const auto& axis) { return scalarVectorOwned(axis); });
  return combine(result, scalarVectorOwned(value.values));
}

Bytes ownedSignature(const LibraryArcSignature& value)
{
  return combine(ownedString(value.library_name), ownedString(value.sdf_condition));
}

Bytes ownedChild(const ChildTimingArcModel& value)
{
  Bytes result = ownedSignature(value.signature);
  for (const auto& table : value.delay_tables)
    result = combine(result, ownedTable(table));
  for (const auto& table : value.slew_tables)
    result = combine(result, ownedTable(table));
  for (const auto& table : value.check_tables)
    result = combine(result, ownedTable(table));
  return result;
}

Bytes ownedTimingEndpoint(const TimingCheckEndpointModel& value)
{
  Bytes result = combine(ownedString(value.instance_name), ownedString(value.clock_pin_raw_name));
  result = combine(result, ownedString(value.data_pin_raw_name));
  result = combine(result, ownedString(value.clock_pin_full_name));
  return combine(result, ownedString(value.data_pin_full_name));
}

Bytes ownedLibraryPort(const LibraryPortModel& value)
{
  return ownedString(value.name);
}

Bytes ownedLibraryCellArc(const LibraryCellArcModel& value)
{
  Bytes result = combine(ownedString(value.source_port), ownedString(value.sink_port));
  return result;
}

Bytes ownedLibraryCheck(const LibraryTimingCheckArcModel& value)
{
  Bytes result = combine(ownedString(value.clock_port), ownedString(value.data_port));
  return result;
}

Bytes ownedLibraryCell(const ReferencedLibraryCellModel& value)
{
  Bytes result = combine(ownedString(value.cell_name), ownedString(value.library_name));
  result = combine(result, ownedString(value.clock_pin_name));
  result = combine(result, ownedString(value.data_pin_name));
  result = combine(result, ownedString(value.output_pin_name));
  result = combine(result, vectorOwned(value.ports, [](const auto& port) { return ownedLibraryPort(port); }));
  result = combine(result, vectorOwned(value.cell_arcs, [](const auto& arc) { return ownedLibraryCellArc(arc); }));
  return combine(result, vectorOwned(value.timing_checks, [](const auto& check) { return ownedLibraryCheck(check); }));
}

Bytes ownedParasiticNode(const ParasiticNodeModel& value)
{
  return combine(ownedString(value.map_key), ownedString(value.embedded_name));
}

Bytes ownedParasiticResistor(const ParasiticResistorModel& value)
{
  return combine(ownedString(value.source), ownedString(value.sink));
}

Bytes ownedParasiticNet(const ParasiticNetModel& value)
{
  Bytes result = combine(ownedString(value.map_key), ownedString(value.embedded_name));
  result = combine(result, vectorOwned(value.nodes, [](const auto& node) { return ownedParasiticNode(node); }));
  return combine(result, vectorOwned(value.resistors, [](const auto& resistor) { return ownedParasiticResistor(resistor); }));
}

Bytes uniqueModelBytes(const CsrReferenceInput& input)
{
  std::vector<const std::vector<ChildTimingArcModel>*> owners;
  const auto append = [&](const auto& model) {
    if (model.childrenOwner())
      owners.push_back(model.childrenOwner().get());
  };
  for (const auto& arc : input.arcs)
    append(arc.transfer);
  for (const auto& cell : input.referenced_library_cells) {
    for (const auto& arc : cell.cell_arcs)
      append(arc.transfer);
    for (const auto& check : cell.timing_checks)
      append(check);
  }
  std::sort(owners.begin(), owners.end(), std::less<>{});
  owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
  Bytes result;
  for (const auto* owner : owners) {
    result = combine(result, {sizeof(*owner), sizeof(*owner)});
    result = combine(result, vectorOwned(*owner, [](const auto& child) { return ownedChild(child); }));
  }
  return result;
}

Bytes clockConstraintDelta(const CsrReferenceInput& input)
{
  Bytes result;
  for (const auto& clock : input.clocks) {
    result = combine(result, ownedString(clock.embedded_clock_name));
    for (const auto& source : clock.ordered_sources)
      result = combine(result, ownedString(source.full_name));
  }
  return result;
}

Bytes portConstraintDelta(const CsrReferenceInput& input)
{
  Bytes result;
  for (const auto& port : input.port_constraints) {
    result = combine(result, ownedString(port.map_key));
    result = combine(result, ownedString(port.embedded_port_name));
  }
  return result;
}

Bytes caseConstraintDelta(const CsrReferenceInput& input)
{
  Bytes result;
  for (const auto& constraint : input.case_analysis_constraints)
    result = combine(result, ownedString(constraint.raw_name));
  return result;
}

Bytes step4OwnedBytes(const CsrReferenceInput& input)
{
  Bytes result = uniqueModelBytes(input);
  result = combine(result, clockConstraintDelta(input));
  result = combine(result, portConstraintDelta(input));
  result = combine(result, caseConstraintDelta(input));
  result = combine(result, ownedString(input.parasitics.capacitance_unit));
  result = combine(result, ownedString(input.parasitics.resistance_unit));
  result
      = combine(result, vectorOwned(input.clock_owner_by_node, [](const std::optional<std::string>& owner) { return owner ? ownedString(*owner) : Bytes{}; }));
  result = combine(result, vectorOwned(input.timing_check_endpoints, [](const auto& endpoint) { return ownedTimingEndpoint(endpoint); }));
  result = combine(result, vectorOwned(input.referenced_library_cells, [](const auto& cell) { return ownedLibraryCell(cell); }));
  return combine(result, vectorOwned(input.parasitics.nets, [](const auto& net) { return ownedParasiticNet(net); }));
}

}  // namespace

CsrSnapshotStorageReport independentReferenceInputStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata)
{
  auto report = independentSnapshotStorage(input, metadata);
  const Bytes step4 = step4OwnedBytes(input);
  report.host_bytes.logical = add(report.host_bytes.logical, step4.logical);
  report.host_bytes.capacity = add(report.host_bytes.capacity, step4.capacity);
  return report;
}

}  // namespace ista::csr::snapshot_test
