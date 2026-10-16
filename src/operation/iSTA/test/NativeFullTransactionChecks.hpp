// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <stdexcept>
#include <filesystem>
#include "BenchmarkTimingConsumer.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "NativeFullEditPreparer.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "PublicTimingJson.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::test {
inline void requireTimingOnlyFiles(const std::filesystem::path& directory)
{
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    const auto ext = entry.path().extension();
    const auto name = entry.path().filename();
    if (entry.is_regular_file()
        && (ext == ".rpt" || ext == ".sdf" || ext == ".bin"
            || name == "qor_summary.json" || name == "timing_paths.json"))
      throw std::runtime_error("timing workload generated a disk artifact: " + entry.path().string());
  }
}
inline void checkNativeFullTransactions(const timing_edit::oracle::ProductionFreshSessionView& view,
                                        const buffer_trace::ReplayProgram& program)
{
  using namespace timing_edit;
  const auto require = [](bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
  };
  const auto rejected = [&](auto&& work) {
    try { work(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid native full transaction was accepted");
  };
  const auto& plans = std::visit([](const auto& workload) -> const std::vector<EditBatch>& { return workload.batches; }, program.workload);
  require(plans.size() >= 2, "transaction checks require two batches");
  NativeTimingEditPreparer::requireBaselineIdentity(view.database, view.reference_input, program.baseline());
  auto state = NativeFullEditPreparer::bindBaseline(view.database, view.authority);
  const auto baseline = csr::canonicalReferenceInputBytes(view.reference_input);
  require(view.database.get_timing_path_group_list().empty(), "timing-only baseline prepared reports eagerly");
  const auto public_baseline = analyzeAndCapturePublicTiming();
  require(!public_baseline.at("paths").at("paths").empty(), "baseline report fixture has no path");
  const auto restored = [&] {
    auto exported = csr::exportCsrReferenceInputCandidate(view.database, view.reference_input.analysis_context, view.session_token);
    require(std::holds_alternative<csr::CsrReferenceInput>(exported), "rollback export failed");
    require(csr::canonicalReferenceInputBytes(std::get<csr::CsrReferenceInput>(exported)) == baseline, "rollback changed baseline input");
    require(TimingReporterProbeAccess::read() == public_baseline, "rollback changed public results");
    require(state.generation().value() == 0 && view.authority.readyForQuery(), "rollback changed session metadata");
  };
  {
    auto pending = NativeFullEditPreparer::prepare(view.database, view.authority, plans[0], state);
    rejected([&] { static_cast<void>(NativeFullEditPreparer::prepare(view.database, view.authority, plans[0], state)); });
    auto moved = std::move(pending);
    require(!pending.commit(), "moved-from batch committed");
    require(moved.transaction().rollback(), "explicit rollback failed");
    require(!moved.commit(), "rolled-back batch committed");
  }
  restored();
  {
    auto abandoned = NativeFullEditPreparer::prepare(view.database, view.authority, plans[0], state);
    NativeCandidateExecutor::executeFull(abandoned.transaction());
  }
  restored();
  if constexpr (incregpusta_debug_checks_enabled) {
    auto failed = NativeFullEditPreparer::prepare(view.database, view.authority, plans[0], state);
    rejected([&] { NativeCandidateExecutor::executeFull(failed.transaction(), NativeExecutionFailpoint::kBeforeTimingPropagatorWork); });
    require(!failed.commit(), "failed computation committed");
  }
  restored();
  Database foreign;
  rejected([&] { static_cast<void>(NativeFullEditPreparer::bindBaseline(foreign, view.authority)); });
  rejected([&] { static_cast<void>(NativeFullEditPreparer::prepare(foreign, view.authority, plans[0], state)); });
  auto first = NativeFullEditPreparer::prepare(view.database, view.authority, plans[0], state);
  NativeCandidateExecutor::executeFull(first.transaction());
  require(first.commit(), "first native full commit failed");
  rejected([&] { static_cast<void>(NativeFullEditPreparer::prepare(view.database, view.authority, plans[0], state)); });
  auto second = NativeFullEditPreparer::prepare(view.database, view.authority, plans[1], state);
  NativeCandidateExecutor::executeFull(second.transaction());
  require(second.commit(), "second native full commit failed");
  require(!first.commit() && !second.commit() && state.generation().value() == 2, "repeated commit changed generation");
  require(TimingReporterProbeAccess::read() == capturePublicTiming(), "stream/file public reports differ");
}
}  // namespace ista::test
