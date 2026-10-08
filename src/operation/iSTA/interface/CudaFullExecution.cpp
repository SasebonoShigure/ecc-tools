// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaFullExecution.hpp"
#include "StaCsrSessionContext.hpp"
#include "NativeFullEditPreparer.hpp"
#include "NativeCandidateExecutor.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "Database.hpp"
#include <chrono>
#include <stdexcept>

namespace ista::csr::detail {
CudaFullStage stageCudaFullBatch(
    StaCsrSessionContext& context, timing_edit::NativeFullEditBatch& native, CudaDualCsrSession& device)
{
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  auto& transaction = native.transaction();
  timing_edit::NativeCandidateExecutor::prepareFullGraph(transaction);
  const auto graph_done = Clock::now();
  auto& database = transaction.database();
  auto exported = exportCsrReferenceInputCandidate(
      database, context.analysisContext(), context.token(), context.libraryModels(database));
  auto* input = std::get_if<CsrReferenceInput>(&exported);
  if (!input) throw std::runtime_error("full CSR export failed");
  const auto export_done = Clock::now();
  std::vector<std::string> names;
  names.reserve(input->nodes.size());
  for (const auto& node : input->nodes) names.push_back(node.key.full_name);
  std::vector<std::optional<std::size_t>> ranks(input->arcs.size());
  for (const auto& arc : input->arcs) ranks.at(arc.id.value()) = arc.native_traversal_rank;
  auto candidate = device.stageFullNativeExport(
      *input, {input->nodes.size(), database.get_net_map().size(), input->arcs.size()});
  if (candidate.computationCode()) throw std::runtime_error("full GPU computation failed");
  CudaFullStage result{std::move(candidate), std::move(names), std::move(ranks), {},
      input->nodes.size(), input->arcs.size(),
      std::chrono::duration<double, std::milli>(graph_done - start).count(),
      std::chrono::duration<double, std::milli>(export_done - graph_done).count()};
  if constexpr (incregpusta_debug_checks_enabled) result.diagnostic_input.emplace(std::move(*input));
  return result;
}
}  // namespace ista::csr::detail
