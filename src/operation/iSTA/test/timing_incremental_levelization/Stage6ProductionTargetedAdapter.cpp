// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "Stage6ProductionTargetedAdapter.hpp"

#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "BufferTraceTypes.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "DualCsrTargetedPhysicalProjection.hpp"
#include "DualCsrTraceReplayer.hpp"
#include "ProductionFreshSessionLoader.hpp"

namespace ista::timing_dual_csr::test {
namespace {

using buffer_trace::OneShotProcessState;
using timing_edit::oracle::FreshBaselineArtifacts;
using timing_edit::oracle::ProductionFreshSessionView;

std::atomic<OneShotProcessState> g_one_shot_state{
    OneShotProcessState::kUnused};
std::atomic<std::uint64_t> g_loader_invocations{0};
std::atomic<std::uint64_t> g_guarded_work_invocations{0};

[[noreturn]] void targetedFail(std::string message)
{
  throw std::runtime_error("Stage 6 targeted adapter: "
                           + std::move(message));
}

struct FinishOneShot
{
  ~FinishOneShot()
  {
    g_one_shot_state.store(OneShotProcessState::kFinished,
                           std::memory_order_release);
  }
};

void requireArtifactEvidence(const ProductionFreshSessionView& view,
                             const FreshBaselineArtifacts& artifacts)
{
  constexpr std::array<std::string_view, 5> roles{
      "tech_lef", "cell_lef", "liberty", "verilog", "sdc"};
  const std::array<std::filesystem::path, 5> paths{
      artifacts.tech_lef, artifacts.cell_lef, artifacts.liberty,
      artifacts.verilog, artifacts.sdc};
  if (view.artifact_evidence.size() != roles.size())
    targetedFail("loader artifact evidence cardinality differs");
  for (std::size_t index = 0; index < roles.size(); ++index) {
    const auto& evidence = view.artifact_evidence[index];
    if (evidence.role != roles[index]
        || evidence.logical_name != paths[index].filename().string()
        || evidence.content_sha256.empty()) {
      targetedFail("loader artifact evidence differs by role/name");
    }
  }
}

void requirePhysicalUnchanged(
    buffer_trace::SourceIsolationFunctionalReceipt& source)
{
  source.physical_after = captureTargetedSourcePhysical();
  if (source.physical_before != source.physical_after)
    targetedFail("targeted work changed source IDM/IDB state");
}

void requireArtifactsUnchanged(
    const DualCsrTargetedArtifactSupport& artifacts,
    buffer_trace::SourceIsolationFunctionalReceipt& source)
{
  source.artifacts_after = artifacts.capture();
  if (source.artifacts_before != source.artifacts_after)
    targetedFail("loader or targeted work changed source artifacts");
}

}  // namespace

OneShotProcessState stage6TargetedOneShotState() noexcept
{
  return g_one_shot_state.load(std::memory_order_acquire);
}

ProductionStage6TargetedPrefix runProductionStage6TargetedPrefix(
    const buffer_trace::ReplayProgram& program,
    std::size_t prefix_batches)
{
  OneShotProcessState expected = OneShotProcessState::kUnused;
  if (!g_one_shot_state.compare_exchange_strong(
          expected, OneShotProcessState::kEntered,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    targetedFail("a targeted process may enter the loader only once");
  }
  FinishOneShot finish;
  const auto violations = buffer_trace::validateReplayProgram(program);
  if (!violations.empty()) {
    targetedFail(violations.front().field + ": "
                 + violations.front().message);
  }

  DualCsrTargetedArtifactSupport artifacts(program);
  buffer_trace::SourceIsolationFunctionalReceipt source;
  source.artifacts_before = artifacts.capture();
  std::optional<DualCsrTraceRunResult> replay;
  std::size_t callback_count = 0;
  g_loader_invocations.fetch_add(1, std::memory_order_acq_rel);
  try {
    timing_edit::oracle::ProductionFreshSessionLoader::run(
        artifacts.artifacts(),
        [&](const ProductionFreshSessionView& view) {
          ++callback_count;
          if (callback_count != 1 || replay) {
            targetedFail("loader invoked guarded targeted work more than once");
          }
          requireArtifactEvidence(view, artifacts.artifacts());
          source.physical_before = captureTargetedSourcePhysical();
          if (!source.physical_before.primary_idb.present)
            targetedFail("production loader exposed no primary IDB");
          try {
            g_guarded_work_invocations.fetch_add(
                1, std::memory_order_acq_rel);
            replay.emplace(replayMixedDualCsrTracePrefix(
                {.baseline_database = view.database,
                 .baseline_reference_input = view.reference_input,
                 .authority = view.authority,
                 .session_token = view.session_token},
                program, prefix_batches));
          } catch (...) {
            requirePhysicalUnchanged(source);
            throw;
          }
          requirePhysicalUnchanged(source);
        });
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    requireArtifactsUnchanged(artifacts, source);
    std::rethrow_exception(failure);
  }
  requireArtifactsUnchanged(artifacts, source);
  if (callback_count != 1 || !replay
      || g_loader_invocations.load(std::memory_order_acquire) != 1
      || g_guarded_work_invocations.load(std::memory_order_acquire) != 1
      || stage6TargetedOneShotState() != OneShotProcessState::kEntered) {
    targetedFail("one-shot targeted loader evidence differs");
  }
  return {
      .replay = std::move(*replay),
      .source_isolation = std::move(source),
      .loader_invocation_count = 1,
      .guarded_work_invocation_count = 1};
}

}  // namespace ista::timing_dual_csr::test
