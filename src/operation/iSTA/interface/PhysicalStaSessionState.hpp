// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <utility>
#include "PhysicalTimingEdit.hpp"
#include "PhysicalPlacementRows.hpp"
#include "PhysicalStaJournal.hpp"
#include "IncrementalStaSession.hpp"
#include "NativeFullEditPreparer.hpp"
#include "DualCsrState.hpp"
#include "StaExecutionContract.hpp"
#if ISTA_ENABLE_CUDA
#include "CudaCsrEditState.hpp"
namespace ista::csr { class CudaDualCsrSession; }
#endif

namespace ista {
class StaCsrSessionContext;
// Internal owner tied to the EngineDB IDB and the existing native session.
// Pending transactions stay on the operation stack, never in this owner.
class PhysicalStaSessionState final
{
 public:
  PhysicalStaSessionState(idb::IdbDesign& design, izh::PreRouteRcTechnology technology, StaExecutionRequest request)
      : design(design), technology(std::move(technology)), request(request) {}
  void bindRecovery(const PhysicalStaConfig& config, Database& database, const csr::StaAnalysisContext& analysis, bool recovery);
  StaExecutionRecord recover(Database& database, StaCsrSessionContext& context, const timing_edit::NativeExecutionAuthority& authority, StaExecutionRecord execution);
  PhysicalStaBatchResult applyBatch(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, const timing_edit::EditBatch& edits,
      std::span<const PhysicalBufferLocation> locations);
  void bindNativeBaseline(Database& database, const timing_edit::NativeExecutionAuthority& authority);
  void bindBaseline(Database& database, const csr::CsrReferenceInput& input, StaCsrSessionContext& context);
  [[nodiscard]] timing_edit::TransactionGeneration generation(const StaCsrSessionContext& context) const;
  void materialize(Database& database, StaCsrSessionContext& context, const timing_edit::NativeExecutionAuthority& authority);
  void prepareReport(Database& database, StaCsrSessionContext& context, const timing_edit::NativeExecutionAuthority& authority);
  PhysicalStaCurrentResult read(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, bool summary_only);

  PhysicalGreedySelection selectBuffers(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, std::span<const std::string> cells,
      std::uint64_t top_k, std::uint64_t max_buffers);
  std::string publishArtifacts(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority);
  std::string readArtifacts(const StaCsrSessionContext& context) const;
  izh::PhysicalPlacementRows& placementRows();

  idb::IdbDesign& design;
  const izh::PreRouteRcTechnology technology;
  const StaExecutionRequest request;
  std::unique_ptr<timing_edit::NativeFullEditState> full;
#if ISTA_ENABLE_CUDA
  std::unique_ptr<CudaCsrEditState> cuda_graph;
#endif
 private:
  void prepareCsrView(Database& database, const timing_edit::NativeExecutionAuthority& authority,
      const timing_dual_csr::DualCsrState& graph, const csr::CsrTimingValues* values);
#if ISTA_ENABLE_CUDA
  PhysicalStaCurrentResult readCsr(Database& database, const timing_edit::NativeExecutionAuthority& authority,
      csr::CudaDualCsrSession& device, bool summary_only);
#endif
  std::pair<std::string, std::string> _csr_report;
  std::optional<timing_edit::TransactionGeneration> _csr_report_generation;
  void materializeCsr(Database& database, const timing_edit::NativeExecutionAuthority& authority,
      const timing_dual_csr::DualCsrState& graph, const csr::CsrTimingValues& values);
  PhysicalStaBatchResult applyBatchImpl(Database& database, StaCsrSessionContext& context,
      const timing_edit::NativeExecutionAuthority& authority, const timing_edit::EditBatch& edits,
      std::span<const PhysicalBufferLocation> locations, bool recovery);
  std::unique_ptr<PhysicalStaJournal> _journal;
  std::unique_ptr<izh::PhysicalPlacementRows> _placement;
  std::string _baseline_id;
  std::filesystem::path _recovery_directory;
  std::optional<timing_edit::TransactionGeneration> _native_generation, _timing_generation, _report_generation;
};
}  // namespace ista
