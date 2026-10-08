// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "CudaDualCsrPatch.hpp"
#include "NativeEditTransaction.hpp"
#include "TimingEditResolver.hpp"

namespace ista::csr::detail { class NativeLibraryModels; }
namespace ista::timing_edit {
// Address/identity metadata only: no host Base slots, Aux edges or arc payloads.
struct NativeTargetedEditState
{
  NativeTargetedEditState() = default;
  NativeTargetedEditState(const NativeTargetedEditState&) = delete;
  NativeTargetedEditState& operator=(const NativeTargetedEditState&) = delete;
  NativeTargetedEditState(NativeTargetedEditState&&) noexcept = default;
  NativeTargetedEditState& operator=(NativeTargetedEditState&&) noexcept = default;
  csr::detail::NativeLibraryModels* models{nullptr};
  TimingEditSessionToken session_token;
  TransactionGeneration generation;
  std::shared_ptr<const BaselineSemanticIdentity> baseline;
  std::shared_ptr<const std::set<csr::ArcId>> clock_arcs;
  TimingEditResolverState resolver;
  std::vector<std::string> node_names;
  std::map<std::string, csr::NodeId> node_ids;
  // Stable map keys owned by resolver. std::map moves/merges preserve them;
  // the containing state is move-only so they cannot point into a copied owner.
  std::vector<const StableArcRef*> arc_refs;
  std::vector<csr::ArcId> native_arc_ids;
  std::vector<std::optional<std::size_t>> arc_native_ranks;
  std::vector<std::uint64_t> base_out_offsets, base_in_offsets;
  // Functional owner/terminal binding; no native timing or graph shadow.
  std::map<csr::NodeId, csr::CsrParasiticBinding> parasitics;
};
struct NativeTargetedEditBatch
{
  NativeEditTransaction transaction;
  NativeTargetedEditState& staged;
  csr::CudaDualCsrPatch patch;
};
class NativeTargetedEditPreparer final
{
 public:
  [[nodiscard]] static NativeTargetedEditState bindBaseline(const Database& database, const csr::CsrReferenceInput& input,
                                                            const BaselineSemanticIdentity& baseline, csr::detail::NativeLibraryModels& models);
  [[nodiscard]] static NativeTargetedEditBatch prepare(Database& database, const NativeExecutionAuthority& authority, EditBatch plan, NativeTargetedEditState& current,
      std::map<std::string, ParasiticNet> prepared_rc = {});
};
}  // namespace ista::timing_edit
