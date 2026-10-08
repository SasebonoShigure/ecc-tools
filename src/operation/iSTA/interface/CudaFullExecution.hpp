// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "CudaDualCsrSession.hpp"

namespace ista {
class StaCsrSessionContext;
namespace timing_edit { class NativeFullEditBatch; }
namespace csr::detail {
struct CudaFullStage {
  CudaDualCsrSession candidate;
  std::vector<std::string> node_names;
  std::vector<std::optional<std::size_t>> arc_ranks;
  std::optional<CsrReferenceInput> diagnostic_input;
  std::size_t node_count{}, arc_count{};
  double native_graph_ms{}, export_ms{};
};
// Builds the native graph, exports inputs and computes the device candidate.
// The caller still owns common transaction qualification and publication.
[[nodiscard]] CudaFullStage stageCudaFullBatch(
    StaCsrSessionContext& context, timing_edit::NativeFullEditBatch& native, CudaDualCsrSession& device);
}
}  // namespace ista
