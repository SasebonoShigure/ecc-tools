// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "StaEditOperation.hpp"
#include "StaCsrSessionContext.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "PowerAnalyzer.hpp"
#include "PowerPropagator.hpp"
#include "PowerReporter.hpp"
#include "TimingReporter.hpp"
#include "SDFWriter.hpp"
#include "StaProductionLifecycle.hpp"
#include "CsrFingerprint.hpp"
#include "Utility.hpp"
#include "json.hpp"
#include "idm.h"
#include "def_write.h"
#include "verilog_write.h"
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace ista {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct ReportConfig {
  Config& config;
  std::string timing, power, sdf;
  int reports, features;
  ReportConfig(Config& value, const fs::path& directory)
      : config(value), timing((directory / "timing").string() + "/"),
        power((directory / "power").string() + "/"), sdf(directory.string() + "/"),
        reports(value.output_timing_reports), features(value.output_timing_features) {
    config.tr_temp_directory_path.swap(timing);
    config.pr_temp_directory_path.swap(power);
    config.sw_temp_directory_path.swap(sdf);
    config.output_timing_reports = 1;
    config.output_timing_features = 1;
  }
  ~ReportConfig() noexcept {
    config.tr_temp_directory_path.swap(timing);
    config.pr_temp_directory_path.swap(power);
    config.sw_temp_directory_path.swap(sdf);
    config.output_timing_reports = reports;
    config.output_timing_features = features;
  }
};
Json reportRequest(const Config& config) {
  return {{"corner", config.timing_corner}, {"max_paths", config.path_report_number},
          {"nworst", config.endpoint_path_report_number}, {"path_limit", config.timing_path_limit},
          {"delay_type", config.timing_report_delay_type}, {"start_end_type", config.timing_report_start_end_type},
          {"max_paths_specified", config.is_path_report_number_specified},
          {"has_slack_less", config.has_timing_report_slack_lesser_than},
          {"slack_less", config.timing_report_slack_lesser_than},
          {"has_slack_greater", config.has_timing_report_slack_greater_than},
          {"slack_greater", config.timing_report_slack_greater_than}};
}
std::string readManifest(const fs::path& path) {
  if (fs::is_symlink(path)) throw std::invalid_argument("physical manifest is a symlink");
  std::ifstream file(path, std::ios::binary);
  file.exceptions(std::ios::badbit);
  if (!file) throw std::runtime_error("physical artifacts have not been published");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}
std::string PhysicalStaSessionState::readArtifacts(const StaCsrSessionContext& context) const
{
  const auto bytes = readManifest(_recovery_directory / "artifacts" / "manifest.json");
  const auto manifest = Json::parse(bytes);
  if (manifest.at("schema_version") != "physical-sta-artifacts/1.0.0"
      || manifest.at("baseline_id") != _baseline_id || manifest.at("generation") != generation(context).value()
      || manifest.at("report_request") != reportRequest(STADM.getConfig()))
    throw std::runtime_error("physical artifact manifest is stale");
  return bytes;
}
std::string PhysicalStaSessionState::publishArtifacts(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority)
{
  prepareReport(database, context, authority);
  const auto current = generation(context).value();
  if (!_journal || !_journal->healthy() || _journal->generation() != current)
    throw std::logic_error("physical artifacts require the current committed journal");
  const auto output = _recovery_directory / "artifacts";
  fs::create_directories(output);
  if (fs::is_symlink(output)) throw std::invalid_argument("physical artifact directory is a symlink");
  auto name = (output / (".generation-" + std::to_string(current) + "-XXXXXX")).string();
  if (!::mkdtemp(name.data())) throw std::system_error(errno, std::generic_category(), "prepare physical artifacts");
  const fs::path staging(name);
  const auto published = output / staging.filename().string().substr(1);
  try {
    fs::create_directory(staging / "timing");
    fs::create_directory(staging / "power");
    ReportConfig config(STADM.getConfig(), staging);
    const auto result = executeStaLifecyclePipeline({
      {"PowerPropagator", [] { PowerPropagator::initInst(); }, [] { STAPP.propagate(); }, [] { PowerPropagator::destroyInst(); }},
      {"PowerAnalyzer", [] { PowerAnalyzer::initInst(); }, [] { STAPA.analyze(); }, [] { PowerAnalyzer::destroyInst(); }},
      {"TimingReporter", [] { TimingReporter::initInst(); }, [] { STATR.report(); }, [] { TimingReporter::destroyInst(); }},
      {"PowerReporter", [] { PowerReporter::initInst(); }, [] { STAPR.report(); }, [] { PowerReporter::destroyInst(); }},
      {"SDFWriter", [] { SDFWriter::initInst(); }, [&] { STASW.write((staging / "design.sdf").string()); }, [] { SDFWriter::destroyInst(); }}
    }, StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
    if (result.cleanup_failed) authority.poison();
    if (!result.succeeded) throw std::runtime_error("physical artifact preparation failed: " + result.message);
    auto* service = dmInst->get_idb_def_service();
    if (!service || service->get_design() != &design)
      throw std::logic_error("physical artifact IDB owner differs");
    idb::DefWrite def_writer(service);
    if (!def_writer.writeDb((staging / "design.def").c_str()))
      throw std::runtime_error("physical DEF output failed");
    std::set<std::string> excluded;
    const auto netlist_path = (staging / "design.v").string();
    idb::VerilogWriter verilog_writer(netlist_path.c_str(), excluded, design, false);
    verilog_writer.writeModule();
    Json files = Json::object();
    for (const auto& entry : fs::recursive_directory_iterator(staging)) {
      if (!entry.is_regular_file()) continue;
      const auto size = entry.file_size();
      std::ifstream file(entry.path(), std::ios::binary);
      file.exceptions(std::ios::badbit);
      if (!file) throw std::runtime_error("physical artifact cannot be read");
      files[fs::relative(entry.path(), staging).generic_string()] = {
        {"size", size}, {"sha256", csr::sha256Hex(file, size)}};
    }
    for (const auto* required : {"design.def", "design.v", "design.sdf", "timing/qor_summary.json",
                                "timing/timing_paths.json", "power/power.rpt", "power/instance_power.bin"})
      if (!files.contains(required) || files.at(required).at("size") == 0)
        throw std::runtime_error("physical artifact is missing or empty: " + std::string(required));
    auto bytes = Json{{"schema_version", "physical-sta-artifacts/1.0.0"}, {"generation", current},
        {"baseline_id", _baseline_id}, {"directory", published.filename().string()},
        {"report_request", reportRequest(config.config)},
        {"files", std::move(files)}}.dump();
    auto manifest = Utility::openOutputFile((staging / "manifest.json").string());
    manifest << bytes;
    manifest.close();
    fs::rename(staging, published);
    // The journal's session lock and operation guard exclude competing writers.
    // All files are complete before this atomic publication; no fsync claim.
    fs::rename(published / "manifest.json", output / "manifest.json");
    return bytes;
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    // A fully prepared but unpublished directory is harmless and kept for inspection.
    throw;
  }
}
std::string IncrementalStaSession::publishPhysicalArtifacts(std::uint64_t expected_generation)
{
  StaEditOperation operation(*this);
  auto& context = operation.context();
  try {
    if (!context._physical || context._physical->generation(context).value() != expected_generation)
      throw std::invalid_argument("physical artifact generation is stale or absent");
    return context._physical->publishArtifacts(STADM.getDatabase(), context, operation.authority());
  } catch (const std::exception& error) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), error.what()), request());
  }
}
std::string IncrementalStaSession::readPhysicalArtifacts(std::uint64_t expected_generation)
{
  StaEditOperation operation(*this);
  auto& context = operation.context();
  try {
    if (!context._physical || context._physical->generation(context).value() != expected_generation)
      throw std::invalid_argument("physical artifact generation is stale or absent");
    return context._physical->readArtifacts(context);
  } catch (const std::exception& error) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), error.what()), request());
  }
}
}  // namespace ista
