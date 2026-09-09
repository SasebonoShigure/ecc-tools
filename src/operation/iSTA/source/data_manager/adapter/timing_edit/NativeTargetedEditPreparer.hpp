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
#include "NativeDatabaseCandidate.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_edit {
// Address/identity metadata only: no host Base slots, Aux edges or arc payloads.
struct NativeTargetedEditState
{
  TimingEditSessionToken session_token;
  TransactionGeneration generation;
  std::shared_ptr<const BaselineSemanticIdentity> baseline;
  std::shared_ptr<const std::set<csr::ArcId>> clock_arcs;
  TimingEditResolverState resolver;
  std::vector<std::string> node_names;
  std::map<std::string, csr::NodeId> node_ids;
  std::vector<csr::ArcId> native_arc_ids;
  std::vector<std::optional<std::size_t>> arc_native_ranks;
  std::vector<std::uint64_t> base_out_offsets, base_in_offsets;
};
struct NativeTargetedEditBatch
{
  NativeDatabaseCandidate candidate;
  NativeTargetedEditState staged;
  csr::CudaDualCsrPatch patch;
};
class NativeTargetedEditPreparer final
{
 public:
  [[nodiscard]] static NativeTargetedEditState bindBaseline(const Database& database, const csr::CsrReferenceInput& input,
                                                            const BaselineSemanticIdentity& baseline);
  [[nodiscard]] static NativeTargetedEditBatch prepare(const Database& database, DesignEditPlan plan, const NativeTargetedEditState& current);
};
}  // namespace ista::timing_edit
