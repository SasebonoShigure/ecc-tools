// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "IndependentFreshBaselineLoader.hpp"

namespace ista {
class Database;
class IncrementalStaSession;
class StaCsrSessionContext;
}

namespace ista::csr {
struct CsrReferenceInput;
}

namespace ista::timing_edit {
class NativeExecutionAuthority;
}

namespace ista::timing_edit::oracle {

struct ProductionFreshSessionView
{
  IncrementalStaSession& session;
  StaCsrSessionContext& session_context;
  Database& database;
  const csr::CsrReferenceInput& reference_input;
  const NativeExecutionAuthority& authority;
  std::uint64_t session_token;
  const std::vector<FreshArtifactEvidence>& artifact_evidence;
};

using ProductionFreshSessionWork = std::function<void(const ProductionFreshSessionView&)>;

// Loads the real artifacts into a fresh CPU/full session, then lends the
// production transaction lane exactly one operation guard covering its whole
// cumulative prefix. No guarded object escapes the callback.
class ProductionFreshSessionLoader final
{
 public:
  static void run(const FreshBaselineArtifacts& artifacts,
                  const ProductionFreshSessionWork& work);
};

}  // namespace ista::timing_edit::oracle
