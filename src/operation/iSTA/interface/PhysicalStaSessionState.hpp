// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "PhysicalTimingEdit.hpp"
#include "PhysicalPlacementRows.hpp"
#include "PhysicalStaJournal.hpp"
#include "IncrementalStaSession.hpp"
#include "NativeFullEditPreparer.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "DualCsrTransactionCoordinator.hpp"
#include "StaExecutionContract.hpp"

namespace ista {
class StaCsrSessionContext;
// Internal owner tied to the EngineDB IDB and the existing native session.
// Pending transactions stay on the operation stack, never in this owner.
class PhysicalStaSessionState final
{
 public:
  PhysicalStaSessionState(idb::IdbDesign& design, izh::PreRouteRcTechnology technology, StaExecutionRequest request)
      : design(design), technology(std::move(technology)), request(request) {}
  void bindRecovery(const PhysicalStaConfig& config, const csr::CsrReferenceInput& input, bool recovery);
  StaExecutionRecord recover(Database& database, StaCsrSessionContext& context, const timing_edit::NativeExecutionAuthority& authority, StaExecutionRecord execution);
  PhysicalStaBatchResult applyBatch(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, const timing_edit::EditBatch& edits,
      std::span<const PhysicalBufferLocation> locations);
  void bindBaseline(Database& database, const csr::CsrReferenceInput& input, StaCsrSessionContext& context);
  [[nodiscard]] timing_edit::TransactionGeneration generation(const StaCsrSessionContext& context) const;
  void materialize(Database& database, StaCsrSessionContext& context, const timing_edit::NativeExecutionAuthority& authority);
  void prepareReport(Database& database, StaCsrSessionContext& context, const timing_edit::NativeExecutionAuthority& authority);
  PhysicalStaCurrentResult read(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, bool summary_only);

  std::string publishArtifacts(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority);
  std::string readArtifacts(const StaCsrSessionContext& context) const;
  izh::PhysicalPlacementRows& placementRows();

  idb::IdbDesign& design;
  const izh::PreRouteRcTechnology technology;
  const StaExecutionRequest request;
  std::unique_ptr<timing_edit::NativeFullEditState> full;
  std::optional<timing_edit::NativeTargetedEditState> cpu_native;
  std::unique_ptr<timing_dual_csr::DualCsrTransactionCoordinator> cpu;
 private:
  PhysicalStaBatchResult applyBatchImpl(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, const timing_edit::EditBatch& edits,
      std::span<const PhysicalBufferLocation> locations, bool recovery);
  std::unique_ptr<PhysicalStaJournal> _journal;
  std::unique_ptr<izh::PhysicalPlacementRows> _placement;
  std::string _baseline_id;
  std::filesystem::path _recovery_directory;
  std::optional<timing_edit::TransactionGeneration> _timing_generation, _report_generation;
};
}  // namespace ista
