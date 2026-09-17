// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "BufferTraceReplayInput.hpp"
#include "DualCsrExactReceiptJson.hpp"
#include "DualCsrFunctionalReceiptJson.hpp"
#include "DualCsrResourceObservationJson.hpp"
#include "DualCsrSourceIsolationJson.hpp"
#include "DualCsrTraceProductionFreshAdapter.hpp"
#include "IncreGpuStaPerformanceJson.hpp"
#include "Stage6ProductionTargetedAdapter.hpp"

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Options = std::map<std::string, std::string>;
using namespace ista;

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error(std::move(message));
}

void writeJson(const fs::path& path, const Json& value)
{
  if (!path.parent_path().empty())
    fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    fail("cannot write " + path.string());
  output << value;
  output.close();
  if (!output)
    fail("failed while writing " + path.string());
}

Options parseOptions(int argc, char* argv[], int begin)
{
  if ((argc - begin) % 2 != 0)
    fail("options must be --name value pairs");
  Options result;
  for (int index = begin; index < argc; index += 2) {
    const std::string key = argv[index];
    if (!key.starts_with("--") || key.size() == 2)
      fail("invalid option " + key);
    if (!result.emplace(key, argv[index + 1]).second)
      fail("duplicate option " + key);
  }
  return result;
}

void requireOptions(const Options& options,
                    std::initializer_list<std::string_view> expected)
{
  std::set<std::string> allowed;
  for (const std::string_view key : expected) {
    allowed.emplace(key);
    if (!options.contains(std::string(key)))
      fail("required option is absent: " + std::string(key));
  }
  for (const auto& [key, value] : options) {
    static_cast<void>(value);
    if (!allowed.contains(key))
      fail("unknown option " + key);
  }
}

std::uint64_t parseU64(std::string_view value, std::string_view label)
{
  std::uint64_t result = 0;
  const auto parsed
      = std::from_chars(value.data(), value.data() + value.size(), result);
  if (value.empty() || parsed.ec != std::errc{}
      || parsed.ptr != value.data() + value.size()) {
    fail(std::string(label) + " must be one canonical unsigned integer");
  }
  return result;
}

buffer_trace::ReplayProgram parseReplay(const Options& options)
{
  const std::string input_flag = options.contains("--replay") ? "--replay" : "--trace";
  requireOptions(options, {input_flag, "--prefix", "--output"});
  return buffer_trace::test::readReplayInput(options.at(input_flag), input_flag == "--replay");
}

Json phaseTimings(
    const std::vector<timing_dual_csr::DualCsrBatchPhaseTimings>& values)
{
  Json result = Json::array();
  for (const auto& value : values) {
    result.push_back({
        {"generation_before", value.generation_before},
        {"generation_after", value.generation_after},
        {"pre_candidate_prepare_ms", value.pre_candidate_prepare_ms},
        {"candidate_apply_execute_ms", value.candidate_apply_execute_ms},
        {"post_candidate_capture_ms", value.post_candidate_capture_ms},
        {"dual_csr_update_ms", value.dual_csr_update_ms},
        {"full_levelization_ms", value.full_levelization_ms},
        {"mixed_build_ms", value.mixed_build_ms},
        {"mixed_kahn_ms", value.mixed_kahn_ms},
        {"reconstruct_ms", value.reconstruct_ms},
        {"full_propagation_ms", value.full_propagation_ms},
        {"invariant_ms", value.invariant_ms},
        {"verify_ms", value.verify_ms},
        {"publish_ms", value.publish_ms}});
  }
  return result;
}

void fresh(const Options& options)
{
  const auto program = parseReplay(options);
  const std::uint64_t prefix = parseU64(options.at("--prefix"), "prefix");
  auto receipt = timing_dual_csr::test::runProductionFreshSemanticPrefix(
      program, static_cast<std::size_t>(prefix));
  Json envelope{{"schema_version", "ista-stage6-trace-process/3.0.0"},
                {"role", "fresh-oracle"},
                {"prefix_batches", prefix},
                {"functional", timing_dual_csr::test::functionalSemanticReceiptToJson(receipt.functional)},
                {"source_isolation", timing_dual_csr::test::sourceIsolationToJson(receipt.source_isolation)},
                {"run",
                 {{"child_pid", static_cast<std::uint64_t>(::getpid())},
                  {"loader_invocation_count", receipt.loader_invocation_count},
                  {"guarded_work_invocation_count", receipt.guarded_work_invocation_count},
                  {"one_shot_state", "finished"}}}};
  writeJson(options.at("--output"), envelope);
}

void targeted(const Options& options)
{
  const auto program = parseReplay(options);
  const std::uint64_t prefix = parseU64(options.at("--prefix"), "prefix");
  auto result = timing_dual_csr::test::runProductionStage6TargetedPrefix(
      program, static_cast<std::size_t>(prefix));
  if (timing_dual_csr::test::stage6TargetedOneShotState()
      != buffer_trace::OneShotProcessState::kFinished) {
    fail("targeted adapter did not finish its one-shot state");
  }

  if constexpr (!incregpusta_debug_checks_enabled) {
    writeJson(options.at("--output"),
              timing_dual_csr::test::performanceResultToJson(result.replay, prefix, phaseTimings(result.replay.envelope.batch_phase_timings)));
    return;
  }

  Json observations = Json::array();
  for (const auto& observation : result.replay.published_observations) {
    observations.push_back(
        {{"functional",
          timing_dual_csr::test::functionalSemanticReceiptToJson(
              observation.functional)},
         {"structural",
          timing_dual_csr::test::structuralReceiptToJson(
              observation.structural)}});
  }
  Json envelope{{"schema_version", "ista-stage6-trace-process/3.0.0"},
                {"role", "targeted-under-test"},
                {"prefix_batches", prefix},
                {"receipt_validated", true},
                {"published_observations", std::move(observations)},
                {"transaction_receipt", timing_dual_csr::test::transactionReceiptToJson(result.replay.transaction_receipt.value())},
                {"source_isolation", timing_dual_csr::test::sourceIsolationToJson(result.source_isolation)},
                {"run",
                 {{"child_pid", static_cast<std::uint64_t>(::getpid())},
                  {"loader_invocation_count", result.loader_invocation_count},
                  {"guarded_work_invocation_count", result.guarded_work_invocation_count},
                  {"one_shot_state", "finished"},
                  {"baseline_factory_calls_outside_targeted", result.replay.envelope.baseline_factory_calls_outside_targeted},
                  {"baseline_factory_calls_inside_targeted", result.replay.envelope.baseline_factory_calls_inside_targeted},
                  {"batch_phase_timings", phaseTimings(result.replay.envelope.batch_phase_timings)},
                  {"batch_resource_observations", timing_dual_csr::test::resourceObservationsToJson(result.replay.envelope.batch_resource_observations)}}}};
  writeJson(options.at("--output"), envelope);
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    if (argc < 2) {
      fail("usage: probe fresh|targeted --trace FILE|--replay FILE --prefix N --output FILE");
    }
    const std::string mode = argv[1];
    if (mode == "build-info" && argc == 2) {
      std::cout << timing_dual_csr::test::incregpustaBuildInfo().dump() << '\n';
      return 0;
    }
    if (mode == "performance" && incregpusta_debug_checks_enabled)
      fail("performance sampling requires ECC_INCREGPUSTA_DEBUG_CHECKS=OFF");
    if (mode != "performance" && !incregpusta_debug_checks_enabled)
      fail("oracle and diagnostic output require ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
    if (mode == "fresh")
      fresh(parseOptions(argc, argv, 2));
    else if (mode == "targeted" || mode == "performance")
      targeted(parseOptions(argc, argv, 2));
    else
      fail("unknown mode " + mode);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Stage 6 trace probe: " << error.what() << '\n';
    return 1;
  }
}
