// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

#include "BufferTraceReplayInput.hpp"
#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrModeRecoveryTestSupport.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "CpuDualCsrBaseline.hpp"
#include "DualCsrTraceReplayer.hpp"
#include "StaCsrSessionContext.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingResultScatterInternal.hpp"
#include "TimingSemanticKey.hpp"
#include "../PublicTimingJson.hpp"

namespace ista::timing_dual_csr::test {
namespace {
using recovery_support::require;

nlohmann::json observeReal(const CommittedDualCsrAnalysisState& state)
{
  auto& database = STADM.getDatabase();
  auto exported = csr::exportCsrReferenceInputCandidate(
      database, state.graph.reference_payload.analysis_context, state.graph.session_token.value());
  const auto* input = std::get_if<csr::CsrReferenceInput>(&exported);
  require(input != nullptr, "real recovery could not export scatter identities");
  std::map<csr::ArcKey, std::size_t, decltype(&csr::arcKeyLess)> native_ranks(csr::arcKeyLess);
  for (const auto& arc : input->arcs)
    native_ranks.emplace(arc.key, arc.native_traversal_rank);
  csr::detail::StableScatterTargets targets;
  for (const auto& node : state.graph.nodes)
    targets.nodes.push_back({&database.get_timing_point_map().at(node.key.full_name), &node.key.full_name});
  targets.arcs.resize(state.graph.arc_slots.size());
  for (std::size_t id = 0; id < state.graph.arc_slots.size(); ++id) {
    const auto& slot = state.graph.arc_slots[id];
    if (slot.active) {
      const auto rank = native_ranks.at(slot.payload.key);
      targets.arcs[id] = {&database.get_arc_list().at(rank), rank};
    }
  }
  targets.timing_order = &database.get_timing_order_list();
  const auto& payload = state.graph.reference_payload;
  auto prepared = csr::detail::prepareResolvedTimingResult(
      targets, {payload.analysis_order, payload.path_source_order, payload.transition_order, payload.clock_tags}, state.timing.values);
  auto* scatter = std::get_if<csr::detail::PreparedTimingResult>(&prepared);
  require(scatter != nullptr, "real recovery committed timing could not scatter");
  csr::detail::publishPreparedTimingResult(*scatter);
  auto functional = projectDualCsrFunctionalSemanticReceipt(state.graph, state.timing);
  require(functional.success(), "real recovery functional projection failed");
  return {{"generation", state.generation.value()},
          {"functional", functionalSemanticReceiptToJson(*functional.receipt)},
          {"public", ista::test::analyzeAndCapturePublicTiming()}};
}
}  // namespace

void realReplayConsumer(const char* replay, const char* output, const char* mode, const char* prefix)
{
  require(std::filesystem::symlink_status(output).type() == std::filesystem::file_type::not_found,
          "consumer output must be a new file");
  const std::string mode_name(mode);
  require(mode_name == "full" || mode_name == "mixed", "consumer mode must be full or mixed");
  std::size_t consumed = 0;
  const std::string prefix_text(prefix);
  const auto prefix_batches = std::stoull(prefix_text, &consumed);
  require(consumed == prefix_text.size() && !prefix_text.empty() && prefix_text.front() != '-',
          "consumer prefix must be an unsigned integer");
  const auto program = buffer_trace::test::readReplayInput(replay, true);
  DualCsrTargetedArtifactSupport artifacts(program);
  nlohmann::json observation;
  timing_edit::oracle::ProductionFreshSessionLoader::run(
      artifacts.artifacts(),
      [&](const timing_edit::oracle::ProductionFreshSessionView& loaded) {
        const DualCsrTraceContext context{
            loaded.database, loaded.reference_input, loaded.authority, loaded.session_token,
            loaded.session_context.libraryModels(loaded.database)};
        const auto run = mode_name == "mixed"
                             ? DualCsrTraceReplayer::replayMixedPrefix(context, program, prefix_batches)
                             : DualCsrTraceReplayer::replayPrefix(context, program, prefix_batches);
        require(run.result.generation == prefix_batches, "consumer observed the wrong generation");
        require(run.result.executed_mode == (mode_name == "mixed" ? LevelizationExecutionMode::kMixedStage6
                                                                  : LevelizationExecutionMode::kFullStage5),
                "consumer replay switched execution mode");
        // Do not analyze or scatter here: the production runner must publish
        // the current results before returning to its public consumers.
        const auto public_timing = ista::test::capturePublicTiming();
        require(public_timing == ista::test::capturePublicTiming(), "repeated public reporting changed results");
        observation = {{"mode", mode_name}, {"generation", run.result.generation}, {"public", public_timing}};
      });
  std::ofstream file(output);
  file << observation << '\n';
  file.close();
  require(bool(file), "consumer output write failed");
}

void realReplayRecovery(const char* replay, const char* output)
{
  require(std::filesystem::symlink_status(output).type() == std::filesystem::file_type::not_found,
          "real recovery output must be a new file");
  const auto program = buffer_trace::test::readReplayInput(replay, true);
  const auto& plans = std::visit([](const auto& workload) -> const auto& { return workload.batches; }, program.workload);
  require(plans.size() >= 2, "real recovery needs a committed batch and continuation");
  std::unique_ptr<DualCsrTransactionCoordinator> coordinator;
  nlohmann::json observations;
  std::uint64_t original_session = 0;
  for (bool recovering : {false, true}) {
    DualCsrTargetedArtifactSupport artifacts(program);
    timing_edit::oracle::ProductionFreshSessionLoader::run(
        artifacts.artifacts(),
        [&](const timing_edit::oracle::ProductionFreshSessionView& loaded) {
          auto baseline = ::ista::timing_dual_csr::makeCpuDualCsrBaseline(
              loaded.database, loaded.reference_input, timing_edit::TimingEditSessionToken{loaded.session_token});
          recovery_support::CumulativeBatchSource source(loaded.authority);
          try {
            if (!recovering) {
              original_session = loaded.session_token;
              coordinator = std::make_unique<DualCsrTransactionCoordinator>(std::move(baseline));
              auto batch = source.mint(*coordinator, plans.front());
              const auto status = coordinator->publish(
                  recovery_support::stage(*coordinator, std::move(batch), LevelizationExecutionMode::kMixedStage6),
                  incregpusta_debug_checks_enabled ? DualCsrPublishFailpoint::kAfterSwap : DualCsrPublishFailpoint::kNone);
              require(status.published && status.poisoned == incregpusta_debug_checks_enabled
                          && (status.ok() || status.code == DualCsrTransactionCode::kPostSwapPoisoned),
                      "real recovery initial mixed commit failed");
              observations["before_recovery"] = observeReal(coordinator->committed());
            } else {
              require(loaded.session_token != original_session, "real recovery reused the old session");
              auto attempt = replayDualCsrModesForRecovery(coordinator->committed(), std::move(baseline), source);
              require(attempt.success(), "real mode recovery failed" + (attempt.failure ? ": " + attempt.failure->detail : std::string{}));
              require(attempt.evidence.has_value() == incregpusta_debug_checks_enabled, "real recovery diagnostic presence differs");
              attempt.evidence.reset();
              if (coordinator->poisoned()) {
                const auto status = coordinator->recover(std::move(*attempt.recovery));
                require(status.ok() && status.published && !status.poisoned, "real recovery could not clear poison");
              } else {
                coordinator = std::make_unique<DualCsrTransactionCoordinator>(std::move(*attempt.recovery));
              }
              observations["after_recovery"] = observeReal(coordinator->committed());
              require(observations["before_recovery"] == observations["after_recovery"], "real recovery changed functional/public state");
              auto& continued = observations["continued"];
              continued = nlohmann::json::array();
              for (std::size_t index = 1; index < plans.size(); ++index) {
                auto batch = source.mint(*coordinator, plans[index]);
                const auto status = coordinator->publish(
                    recovery_support::stage(*coordinator, std::move(batch), LevelizationExecutionMode::kMixedStage6));
                require(status.ok() && status.published && !status.poisoned, "real recovery continuation failed");
                require(coordinator->committed().execution_trace.back().mode() == LevelizationExecutionMode::kMixedStage6,
                        "real continuation silently switched to full");
                continued.push_back(observeReal(coordinator->committed()));
              }
            }
            source.commitPending(coordinator->committed().generation);
          } catch (...) {
            require(source.rollback(), "real recovery failure rollback failed");
            throw;
          }
        });
  }
  observations["restored_prefix_batches"] = 1;
  observations["continued_batches"] = plans.size() - 1;
  std::ofstream file(output);
  file << observations << '\n';
  file.close();
  require(bool(file), "real recovery output write failed");
}
}  // namespace ista::timing_dual_csr::test
