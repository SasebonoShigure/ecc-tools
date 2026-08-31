// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <string_view>

#include "TimingEditTypes.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit {

class TimingEditResolverState;

struct TimingEditPrepareContext
{
  TimingEditSessionToken session_token;
  BaselineSemanticIdentity session_baseline;
  CommittedEditIdentity current_identity;
  csr::StaAnalysisContext analysis_context;
};

// The only native DesignEditPlan -> PreparedBatch expansion path. It captures
// the active native state again before producing any mutation artifact, then
// derives both deltas and the append-only resolver plan from that exact state.
class NativeTimingEditPreparer final
{
 public:
  [[nodiscard]] static CommittedEditIdentity captureCurrentIdentity(const Database& active_database,
                                                                    TransactionGeneration generation,
                                                                    const csr::StaAnalysisContext& analysis_context,
                                                                    TimingEditSessionToken session_token);

  [[nodiscard]] static bool matchesDesignName(Database& active_database, std::string_view expected_design_name);

  // Throws when the resolver does not provide a one-to-one live/tombstone
  // projection over the exact current native Database and CSR arc set.
  static void requireExactResolverCoverage(const Database& active_database, const csr::CsrReferenceInput& current_input,
                                           const TimingEditResolverState& resolver);

  // Binds every durable semantic edit to its materialized raw Instance/Pin/Net
  // closure, rejects unbound native objects, then checks exact resolver-to-CSR
  // coverage. The trace is semantic input, never a hash proxy.
  static void requireExactCommittedTraceClosure(const Database& active_database,
                                                const csr::CsrReferenceInput& current_input,
                                                const TimingEditResolverState& resolver,
                                                const BaselineSemanticIdentity& baseline,
                                                const std::vector<DesignEditPlan>& committed_trace);

  [[nodiscard]] static PreparedBatch prepare(const Database& active_database, DesignEditPlan plan,
                                             const TimingEditPrepareContext& context,
                                             const TimingEditResolverState& resolver);
};

}  // namespace ista::timing_edit
