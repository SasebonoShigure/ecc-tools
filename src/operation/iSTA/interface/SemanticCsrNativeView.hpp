// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <span>
#include "TimingEditTypes.hpp"

namespace ista {
class Database;
namespace csr { struct CsrTimingValues; }
namespace timing_dual_csr { struct DualCsrState; }
namespace timing_edit { class NativeExecutionAuthority; class TimingEditResolverState; }

// Derived native view for unplaced semantic traces; the source IDB is unchanged.
class SemanticCsrNativeView final
{
 public:
  explicit SemanticCsrNativeView(timing_edit::TimingEditSessionToken session) : _session(session) {}
  void materialize(Database& database, const timing_edit::NativeExecutionAuthority& authority,
      const timing_dual_csr::DualCsrState& graph, const timing_edit::TimingEditResolverState& resolver,
      std::span<const timing_edit::EditBatch> committed_batches, const csr::CsrTimingValues& values);

 private:
  timing_edit::TimingEditSessionToken _session;
  timing_edit::TransactionGeneration _generation{};
};
}  // namespace ista
