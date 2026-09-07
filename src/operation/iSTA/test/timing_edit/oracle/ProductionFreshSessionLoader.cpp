// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "ProductionFreshSessionLoader.hpp"

#include <any>
#include <exception>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "CsrFingerprint.hpp"
#include "DataManager.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "IncrementalStaSession.hpp"
#include "STAInterface.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingGraphCsrAdapter.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include "idm.h"
#pragma GCC diagnostic pop

namespace ista::timing_edit::oracle {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("production fresh session loader: " + std::move(message));
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

std::vector<std::uint8_t> readBytes(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail("cannot open source artifact");
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) {
    fail("cannot size source artifact");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
    fail("cannot read source artifact");
  }
  return bytes;
}

FreshArtifactEvidence evidence(std::string role, const fs::path& path)
{
  const std::vector<std::uint8_t> bytes = readBytes(path);
  return {std::move(role), path.filename().string(),
          csr::sha256Hex(std::span<const std::uint8_t>(bytes))};
}

const csr::StaAnalysisContext& analysisContext()
{
  static const csr::StaAnalysisContext context{"slow", "25", "__no_rc_corner__"};
  return context;
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
  if (record.requested_backend != StaBackend::kCpu
      || record.requested_update_strategy != StaUpdateStrategy::kFull
      || record.fallback_policy != StaFallbackPolicy::kError
      || record.executed_backend != StaBackend::kCpu
      || record.executed_update_strategy != StaUpdateStrategy::kFull
      || record.execution_kind != StaExecutionKind::kFullUpdate
      || record.propagation_scope != StaPropagationScope::kFullGraph
      || record.incremental_batch_executed || record.fallback_to_full
      || record.fallback_reason) {
    fail("native baseline did not execute the exact CPU/full contract");
  }
}

void loadArtifacts(const FreshBaselineArtifacts& artifacts)
{
  if (!dmInst->readLef(std::vector<std::string>{artifacts.tech_lef.string()}, true)
      || !dmInst->readLef(std::vector<std::string>{artifacts.cell_lef.string()})) {
    fail("LEF load failed");
  }
  dmInst->get_config().set_sdc_path(artifacts.sdc.string());
  if (!dmInst->readVerilog(artifacts.verilog.string(), artifacts.top)
      || !dmInst->readLib(std::vector<std::string>{artifacts.liberty.string()})) {
    fail("Verilog or Liberty load failed");
  }
}

}  // namespace

void ProductionFreshSessionLoader::run(const FreshBaselineArtifacts& supplied,
                                       const ProductionFreshSessionWork& work)
{
  if (!work) {
    fail("guarded work callback is empty");
  }
  if (supplied.top.empty()) {
    fail("top name is empty");
  }

  FreshBaselineArtifacts artifacts = supplied;
  artifacts.tech_lef = requireRegularFile(supplied.tech_lef, "tech_lef");
  artifacts.cell_lef = requireRegularFile(supplied.cell_lef, "cell_lef");
  artifacts.liberty = requireRegularFile(supplied.liberty, "liberty");
  artifacts.verilog = requireRegularFile(supplied.verilog, "verilog");
  artifacts.sdc = requireRegularFile(supplied.sdc, "sdc");
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
  if constexpr (incregpusta_debug_checks_enabled)
    artifact_evidence = {evidence("tech_lef", artifacts.tech_lef), evidence("cell_lef", artifacts.cell_lef), evidence("liberty", artifacts.liberty),
                         evidence("verilog", artifacts.verilog), evidence("sdc", artifacts.sdc)};

  loadArtifacts(artifacts);
  IncrementalStaSession session(
      {StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError});
  throwOnStaFailure(session.initialize(analysisContext(), makeConfig(artifacts.run_directory)),
                    session.request());
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
    const StaStatus guarded = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
        session, [&](std::uint64_t guarded_token, StaCsrSessionContext& context,
                     const NativeExecutionAuthority& authority) {
          csr::CsrReferenceInputExportResult exported = csr::exportCsrReferenceInputCandidate(
              STADM.getDatabase(), analysisContext(), guarded_token);
          const auto* reference = std::get_if<csr::CsrReferenceInput>(&exported);
          if (reference == nullptr || reference->source_identity.session_token != guarded_token
              || reference->source_identity.graph_generation != 0
              || !reference->unsupported_reasons.empty()) {
            fail("fresh analyzed baseline is not a supported generation-zero CSR reference input");
          }
          work(ProductionFreshSessionView{session, context, STADM.getDatabase(), *reference,
                                          authority, guarded_token, artifact_evidence});
          return StaStatus::success(StaSessionState::kBaselineExecuted);
        });
    if (!guarded.ok()) {
      fail("single operation guard failed: " + guarded.message);
    }
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
