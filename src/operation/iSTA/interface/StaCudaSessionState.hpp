// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <optional>
#include "CudaDualCsrSession.hpp"
#include "NativeTargetedEditPreparer.hpp"

namespace ista {
// Internal session-owned state. All access is under the session operation guard.
class StaCudaSessionState final
{
 public:
  void bindEdits(Database& database, const csr::CsrReferenceInput& input,
                 const timing_edit::BaselineSemanticIdentity& baseline, csr::detail::NativeLibraryModels& models);
  void committed() noexcept;
  void committedFull(std::vector<std::string>&& names, std::vector<std::optional<std::size_t>>&& ranks) noexcept;
  [[nodiscard]] std::uint64_t token() const noexcept { return _token; }

  csr::CudaDualCsrSession device;
  std::optional<timing_edit::NativeTargetedEditState> native;

 private:
  friend class STAInterface;
  StaCudaSessionState(Database& database, const csr::CsrReferenceInput& input);
  friend class StaCsrSessionContext;
  void materialize(Database& database);
  // Caller owns DelayCalculator runtime as well as the session operation guard.
  void prepareReport(Database& database);
  bool _poisoned{false};
  const std::uint64_t _token;
  std::array<csr::AnalysisMode, csr::kAnalysisCount> _analysis_order;
  std::array<csr::PathSource, csr::kPathSourceCount> _path_source_order;
  std::array<csr::Transition, csr::kTransitionCount> _transition_order;
  std::vector<std::string> _clock_tags;
  std::vector<std::string> _baseline_names;
  std::vector<std::optional<std::size_t>> _baseline_ranks;
  std::optional<timing_edit::TransactionGeneration> _timing_generation, _report_generation;
};
}  // namespace ista
