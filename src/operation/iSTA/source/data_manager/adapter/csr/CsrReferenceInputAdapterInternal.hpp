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
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "CsrGraphTypes.hpp"
#include "CsrReferenceTypes.hpp"
#include "Database.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::csr::detail {

class ReferenceExportAbort final : public std::exception
{
 public:
  explicit ReferenceExportAbort(CsrCaptureCode code) : _code(code) {}

  [[nodiscard]] CsrCaptureCode code() const { return _code; }
  [[nodiscard]] const char* what() const noexcept override { return "CSR reference input export rejected native data"; }

 private:
  CsrCaptureCode _code;
};
struct ReferenceExportProgress
{
  CsrReferenceInputExportPhase failed_phase{CsrReferenceInputExportPhase::kAnalysisContextPreflight};
  CsrCurrentFingerprints current_fingerprints;

  [[nodiscard]] CsrReferenceInputExportFailure failure(CsrCaptureCode code) const;
};

[[noreturn]] void abortReferenceExport(CsrCaptureCode code);
[[noreturn]] void malformedReferenceInput();
[[noreturn]] void illegalReferenceNumeric();
[[noreturn]] void invalidReferenceTable();

void requireFiniteReference(double value);
void requireNonnegativeReference(double value);
[[nodiscard]] bool referenceByteStringLess(const std::string& lhs, const std::string& rhs);

struct ReferenceBuildState
{
  ReferenceBuildState(const ista::Database& native_database, CsrGraphSnapshot graph, StaAnalysisContext context);

  const ista::Database& database;
  CsrReferenceInput input;
  std::map<std::string, NodeId> node_ids_by_full_name;
  std::map<std::string, std::size_t> instance_indices_by_name;
  std::map<std::string, std::size_t> referenced_cell_indices_by_name;
  std::vector<std::optional<std::size_t>> port_constraint_indices_by_node;
  std::vector<std::optional<std::size_t>> capacitance_indices_by_node;
  std::vector<std::optional<std::size_t>> clock_owner_by_node;
  std::vector<std::uint8_t> clock_source_nodes;
  std::vector<std::uint8_t> clock_reachable_nodes;
  std::vector<std::uint8_t> replayed_clock_arcs;
  std::vector<std::uint8_t> timing_check_data_nodes;
};

[[nodiscard]] std::optional<NodeId> resolveNode(const ReferenceBuildState& state, const std::string& full_name);
[[nodiscard]] const NodeRecord& requireNode(const ReferenceBuildState& state, NodeId node);
[[nodiscard]] const InstanceRecord* findInstanceRecord(const ReferenceBuildState& state, const std::string& instance_name);
[[nodiscard]] const ista::Instance& requireNativeInstance(const ReferenceBuildState& state, const std::string& instance_name);
[[nodiscard]] const ista::TimingCell* findNativeTimingCell(const ReferenceBuildState& state, const std::string& cell_name);
[[nodiscard]] bool nativeArcMatchesReferenceRecord(const ista::Arc& native_arc, const ArcRecord& arc, const NodeRecord& source, const NodeRecord& sink);
[[nodiscard]] const ista::Arc& requireNativeArc(const ReferenceBuildState& state, const ArcRecord& arc);

[[nodiscard]] Transition convertReferenceTransition(ista::TransType transition);
[[nodiscard]] TimingSense convertReferenceTimingSense(ista::TimingArcSense sense);
[[nodiscard]] TableVariable convertReferenceTableVariable(ista::TimingTableVariableType variable);
[[nodiscard]] TimingCheckKind convertReferenceTimingCheckKind(ista::TimingCheckType kind);
[[nodiscard]] PinDirection convertReferencePortDirection(bool is_input, bool is_output);
[[nodiscard]] TimingTableModel exportReferenceTimingTable(const ista::TimingTable& table);
[[nodiscard]] LibraryArcSignature exportReferenceLibraryArcSignature(const ista::TimingArc& arc);
[[nodiscard]] ChildTimingArcModel exportReferenceChildTimingArc(const ista::TimingArc& arc);
[[nodiscard]] CellTransferModel exportReferenceCellTransfer(const ista::TimingCellArc& cell_arc);

void exportReferenceConstraints(ReferenceBuildState& state);
void exportReferenceParasitics(ReferenceBuildState& state);
void exportReferenceLiberty(ReferenceBuildState& state);
void exportReferenceTimingChecks(ReferenceBuildState& state);
void exportReferenceClockSeeds(ReferenceBuildState& state);
void exportReferenceLoadsAndSupport(ReferenceBuildState& state);

}  // namespace ista::csr::detail
