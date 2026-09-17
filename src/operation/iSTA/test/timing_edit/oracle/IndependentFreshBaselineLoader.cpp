// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "IndependentFreshBaselineLoader.hpp"

#include <any>
#include <exception>
#include <fstream>
#include <map>
#include <span>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "CsrFingerprint.hpp"
#include "DataManager.hpp"
#include "IncrementalStaSession.hpp"
#include "STAInterface.hpp"
#include "TimingGraphCsrAdapter.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include "idm.h"
#pragma GCC diagnostic pop

namespace ista {

// Test-only access is defined in this translation unit so no interface or IDM
// dependency escapes IndependentFreshBaselineLoader.
class StaCsrSessionProbeAccess final
{
 public:
  static void use(std::uint64_t token, const std::function<void(Database&)>& callback)
  {
    StaStatus failure;
    auto guard = STAInterface::getInst().acquireUse(StaOwner::session(token), nullptr, false,
                                                    StaSessionState::kBaselineExecuted, &failure);
    if (!guard) {
      throw std::runtime_error("independent oracle failed to acquire session operation guard: " + failure.message);
    }
    callback(STADM.getDatabase());
  }
};

}  // namespace ista

namespace ista::timing_edit::oracle {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(std::string_view message)
{
  throw std::runtime_error("independent fresh baseline loader: " + std::string(message));
}

fs::path requireRegularFile(const fs::path& path, std::string_view role)
{
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (error || status.type() != fs::file_type::regular) {
    fail(std::string(role) + " must name a regular non-symlink artifact");
  }
  const fs::path canonical = fs::canonical(path, error);
  if (error || !canonical.is_absolute()) {
    fail(std::string(role) + " cannot be canonicalized");
  }
  return canonical;
}

FreshArtifactEvidence evidence(const FreshArtifactFile& file)
{
  std::ifstream input(file.path, std::ios::binary);
  if (!input)
    fail("cannot open source artifact");
  try {
    return {file.role, file.logical_name, csr::sha256Hex(input, fs::file_size(file.path))};
  } catch (const std::exception& error) {
    fail(error.what());
  }
}

std::map<std::string, std::any> makeConfig(const fs::path& run_directory)
{
  return {{"-temp_directory_path", (run_directory / "sta").string()},
          {"-thread_number", std::int32_t{1}},
          {"-output_timing_reports", std::int32_t{0}},
          {"-output_timing_features", std::int32_t{0}},
          {"-timing_path_limit", std::int32_t{20}},
          {"-max_paths", std::int32_t{20}},
          {"-nworst", std::int32_t{20}},
          {"-timing_corner", std::string{}}};
}

void requireCpuFullRecord(const StaExecutionRecord& record)
{
  if (record.requested_backend != StaBackend::kCpu || record.requested_update_strategy != StaUpdateStrategy::kFull
      || record.fallback_policy != StaFallbackPolicy::kError || record.executed_backend != StaBackend::kCpu
      || record.executed_update_strategy != StaUpdateStrategy::kFull || record.execution_kind != StaExecutionKind::kFullUpdate
      || record.propagation_scope != StaPropagationScope::kFullGraph || record.incremental_batch_executed
      || record.fallback_to_full || record.fallback_reason) {
    fail("native baseline did not execute the exact CPU/full contract");
  }
}

void loadArtifacts(const FreshBaselineArtifacts& artifacts)
{
  if (!dmInst->readLef(std::vector<std::string>{artifacts.tech_lef.string()}, true)
      || !dmInst->readLef(freshArtifactPathStrings(artifacts.cell_lefs))) {
    fail("LEF load failed");
  }
  dmInst->get_config().set_sdc_path(artifacts.sdc.string());
  if (!dmInst->readVerilog(artifacts.verilog.string(), artifacts.top)
      || !dmInst->readLib(freshArtifactPathStrings(artifacts.liberties))) {
    fail("Verilog or Liberty load failed");
  }
}

}  // namespace

void IndependentFreshBaselineLoader::run(const FreshBaselineArtifacts& supplied, const FreshBaselineWork& work)
{
  if (!work) {
    fail("guarded work callback is empty");
  }
  if (supplied.top.empty()) {
    fail("top name is empty");
  }

  FreshBaselineArtifacts artifacts = supplied;
  artifacts.tech_lef = requireRegularFile(supplied.tech_lef, "tech_lef");
  if (artifacts.cell_lefs.empty() || artifacts.liberties.empty())
    fail("cell LEF and Liberty lists must be nonempty");
  for (auto& path : artifacts.cell_lefs)
    path = requireRegularFile(path, "cell_lef");
  for (auto& path : artifacts.liberties)
    path = requireRegularFile(path, "liberty");
  artifacts.verilog = requireRegularFile(supplied.verilog, "verilog");
  artifacts.sdc = requireRegularFile(supplied.sdc, "sdc");
  std::set<fs::path> unique_paths;
  for (const auto& file : freshArtifactFiles(artifacts))
    if (!unique_paths.insert(file.path).second)
      fail("source artifacts must have distinct canonical paths");
  if (artifacts.analysis_context.liberty_corner.empty() || artifacts.analysis_context.temperature.empty()
      || artifacts.analysis_context.rc_corner.empty())
    fail("analysis context fields must be nonempty");
  std::error_code directory_error;
  fs::create_directories(artifacts.run_directory, directory_error);
  if (directory_error || !fs::is_directory(artifacts.run_directory)) {
    fail("run directory is unavailable");
  }
  artifacts.run_directory = fs::canonical(artifacts.run_directory, directory_error);
  if (directory_error) {
    fail("run directory cannot be canonicalized");
  }

  std::vector<FreshArtifactEvidence> artifact_evidence;
    for (const auto& file : freshArtifactFiles(artifacts))
      artifact_evidence.push_back(evidence(file));

  loadArtifacts(artifacts);
  IncrementalStaSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError});
  throwOnStaFailure(session.initialize(artifacts.analysis_context, makeConfig(artifacts.run_directory)), session.request());
  const auto token = session.sessionToken();
  if (!token || *token == 0 || session.state() != StaSessionState::kInitialized) {
    fail("initialized session token/state differs");
  }
  requireCpuFullRecord(requireStaExecutionRecord(session.runFullBaseline(), session.request()));
  if (session.state() != StaSessionState::kBaselineExecuted) {
    fail("baseline session state differs");
  }

  std::exception_ptr work_failure;
  try {
    StaCsrSessionProbeAccess::use(*token, [&](Database& database) {
      csr::CsrReferenceInputExportResult exported
          = csr::exportCsrReferenceInputCandidate(database, artifacts.analysis_context, *token);
      const auto* reference = std::get_if<csr::CsrReferenceInput>(&exported);
      if (reference == nullptr || reference->source_identity.session_token != *token
          || reference->source_identity.graph_generation != 0 || !reference->unsupported_reasons.empty()) {
        fail("fresh analyzed baseline is not a supported generation-zero CSR reference input");
      }
      work(FreshBaselineView{database, *reference, *token, artifact_evidence});
    });
  } catch (...) {
    work_failure = std::current_exception();
  }

  try {
    throwOnStaFailure(session.close(), session.request());
    STAInterface::destroyInst();
  } catch (...) {
    if (!work_failure) {
      throw;
    }
  }
  if (work_failure) {
    std::rethrow_exception(work_failure);
  }
}

}  // namespace ista::timing_edit::oracle
