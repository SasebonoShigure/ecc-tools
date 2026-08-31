// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <unistd.h>

#include "BufferFixedTraceGenerator.hpp"
#include "BufferTraceJson.hpp"
#include "BufferTraceProductionFreshAdapter.hpp"
#include "BufferTraceProductionGainValidator.hpp"
#include "BufferTraceReceiptJson.hpp"
#include "BufferTraceReplayer.hpp"
#include "json.hpp"

namespace ista::buffer_trace::test {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Options = std::map<std::string, std::string>;

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("buffer trace production probe: " + std::move(message));
}

std::string readText(const fs::path& path)
{
  std::error_code error;
  const std::uintmax_t file_size = fs::file_size(path, error);
  if (error
      || file_size
             > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::streamsize>::max())) {
    fail("cannot measure input " + path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail("cannot open input " + path.string());
  }
  std::string value(static_cast<std::size_t>(file_size), '\0');
  if (!value.empty()) {
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
  }
  if (input.gcount() != static_cast<std::streamsize>(value.size())
      || input.bad()
      || input.peek() != std::char_traits<char>::eof()) {
    fail("cannot read input " + path.string());
  }
  return value;
}

void writeText(const fs::path& path, std::string_view value)
{
  const fs::path parent = path.parent_path();
  std::error_code error;
  if (!parent.empty()) {
    fs::create_directories(parent, error);
    if (error) {
      fail("cannot create output directory " + parent.string());
    }
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    fail("cannot open output " + path.string());
  }
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  output.put('\n');
  output.close();
  if (!output) {
    fail("cannot write output " + path.string());
  }
}

Options parseOptions(int argc, char* argv[], std::string& mode)
{
  if (argc < 2) {
    fail("missing mode");
  }
  mode = argv[1];
  Options result;
  for (int index = 2; index < argc; index += 2) {
    if (index + 1 >= argc) {
      fail("option is missing a value");
    }
    const std::string key = argv[index];
    if (!key.starts_with("--") || key.size() == 2) {
      fail("unknown positional argument " + key);
    }
    if (!result.emplace(key, argv[index + 1]).second) {
      fail("duplicate option " + key);
    }
  }
  return result;
}

void requireOptions(const Options& options,
                    std::initializer_list<std::string_view> expected)
{
  std::set<std::string> allowed;
  for (std::string_view key : expected) {
    allowed.emplace(key);
    if (!options.contains(std::string(key))) {
      fail("required option is absent: " + std::string(key));
    }
  }
  for (const auto& [key, value] : options) {
    static_cast<void>(value);
    if (!allowed.contains(key)) {
      fail("unknown option " + key);
    }
  }
}

std::uint64_t parseU64(std::string_view value, std::string_view label)
{
  std::uint64_t result = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto parsed = std::from_chars(begin, end, result);
  if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    fail(std::string(label) + " must be one canonical unsigned integer");
  }
  return result;
}

template <typename Result, typename Value>
Value requireParsed(Result parsed, std::string_view label)
{
  if (const auto* violations
      = std::get_if<BufferTraceContractViolations>(&parsed)) {
    if (violations->empty()) {
      fail(std::string(label) + " parse failed without a violation");
    }
    fail(std::string(label) + " rejected at " + violations->front().field
         + ": " + violations->front().message);
  }
  return std::get<Value>(std::move(parsed));
}

const char* stateToken(OneShotProcessState state)
{
  switch (state) {
    case OneShotProcessState::kUnused:
      return "unused";
    case OneShotProcessState::kEntered:
      return "entered";
    case OneShotProcessState::kFinished:
      return "finished";
  }
  fail("unknown one-shot state");
}

std::string bytesHex(std::string_view value)
{
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

Json artifact(const SourceArtifactFunctionalRecord& value)
{
  return {{"role", value.role},
          {"logical_name", value.logical_name},
          {"canonical_path", value.canonical_path},
          {"size", value.size},
          {"bytes_hex", bytesHex(value.exact_bytes)}};
}

Json sourcePin(const SourcePinFunctionalRecord& value)
{
  return {{"name", value.name},
          {"instance", value.instance},
          {"net", value.net},
          {"direction", value.direction},
          {"io", value.io}};
}

Json sourceInstance(const SourceInstanceFunctionalRecord& value)
{
  Json pins = Json::array();
  for (const auto& pin : value.pins) {
    pins.push_back(sourcePin(pin));
  }
  return {{"name", value.name},
          {"master", value.master},
          {"type", value.type},
          {"status", value.status},
          {"orient", value.orient},
          {"x", value.x},
          {"y", value.y},
          {"pins", std::move(pins)}};
}

Json sourceSegment(const SourceWireSegmentFunctionalRecord& value)
{
  Json points = Json::array();
  for (const auto& point : value.points) {
    points.push_back({point[0], point[1], point[2]});
  }
  Json rect = nullptr;
  if (value.delta_rect) {
    rect = Json::array(
        {(*value.delta_rect)[0], (*value.delta_rect)[1],
         (*value.delta_rect)[2], (*value.delta_rect)[3]});
  }
  return {{"layer", value.layer},
          {"new_layer", value.new_layer},
          {"wire", value.wire},
          {"via", value.via},
          {"rect", value.rect},
          {"points", std::move(points)},
          {"vias", value.vias},
          {"delta_rect", std::move(rect)}};
}

Json sourceWire(const SourceWireFunctionalRecord& value)
{
  Json segments = Json::array();
  for (const auto& segment : value.segments) {
    segments.push_back(sourceSegment(segment));
  }
  return {{"statement", value.statement},
          {"shield", value.shield},
          {"segments", std::move(segments)}};
}

Json sourceNet(const SourceNetFunctionalRecord& value)
{
  Json wires = Json::array();
  for (const auto& wire : value.wires) {
    wires.push_back(sourceWire(wire));
  }
  return {{"name", value.name},
          {"type", value.type},
          {"driver", value.driver},
          {"loads", value.loads},
          {"wires", std::move(wires)}};
}

Json idb(const SourceIdbFunctionalProjection& value)
{
  Json instances = Json::array();
  for (const auto& instance : value.instances) {
    instances.push_back(sourceInstance(instance));
  }
  Json pins = Json::array();
  for (const auto& pin : value.io_pins) {
    pins.push_back(sourcePin(pin));
  }
  Json nets = Json::array();
  for (const auto& net : value.nets) {
    nets.push_back(sourceNet(net));
  }
  return {{"present", value.present},
          {"design_name", value.design_name},
          {"instances", std::move(instances)},
          {"io_pins", std::move(pins)},
          {"nets", std::move(nets)},
          {"connected_pin_count", value.connected_pin_count},
          {"placed_instance_count", value.placed_instance_count},
          {"fixed_instance_count", value.fixed_instance_count},
          {"unplaced_instance_count", value.unplaced_instance_count},
          {"wire_count", value.wire_count},
          {"segment_count", value.segment_count},
          {"wire_point_count", value.wire_point_count},
          {"via_count", value.via_count},
          {"wire_length", value.wire_length}};
}

Json physical(const SourcePhysicalFunctionalReceipt& value)
{
  return {{"primary_idb", idb(value.primary_idb)},
          {"optional_netlist_view", idb(value.optional_netlist_view)},
          {"spef_reader_present", value.spef_reader_present}};
}

Json sourceIsolation(const SourceIsolationFunctionalReceipt& value)
{
  Json before = Json::array();
  Json after = Json::array();
  for (const auto& item : value.artifacts_before) {
    before.push_back(artifact(item));
  }
  for (const auto& item : value.artifacts_after) {
    after.push_back(artifact(item));
  }
  const bool unchanged = value.artifacts_before == value.artifacts_after
                         && value.physical_before == value.physical_after;
  if (!unchanged) {
    fail("adapter returned changed source evidence");
  }
  return {{"idb_before", physical(value.physical_before)},
          {"idb_after", physical(value.physical_after)},
          {"artifacts_before", std::move(before)},
          {"artifacts_after", std::move(after)},
          {"source_unchanged", true}};
}

Json derivedTimingFreshness(
    const DerivedTimingFreshnessFunctionalReceipt& value)
{
  return {{"exercised", value.exercised},
          {"timing_point", value.timing_point},
          {"baseline_arrival", value.baseline_arrival},
          {"tampered_arrival", value.tampered_arrival},
          {"recovered_arrival", value.recovered_arrival},
          {"csr_node_count", value.csr_node_count},
          {"csr_arc_count", value.csr_arc_count},
          {"tamper_observed", value.tamper_observed},
          {"rerun_input_exact", value.rerun_input_exact},
          {"rerun_cpu_exact", value.rerun_cpu_exact},
          {"rerun_identity_exact", value.rerun_identity_exact},
          {"rollback_owner_restored", value.rollback_owner_restored},
          {"rollback_input_exact", value.rollback_input_exact},
          {"rollback_cpu_exact", value.rollback_cpu_exact},
          {"rollback_identity_exact", value.rollback_identity_exact},
          {"session_remained_baseline_executed",
           value.session_remained_baseline_executed},
          {"authority_remained_active", value.authority_remained_active}};
}

ReplayReceipt runReplay(BufferTraceProductionFreshAdapter& adapter,
                        const ReplayProgram& program, std::size_t prefix)
{
  return adapter.withFreshBaseline(
      program, [&](BufferReplayContext& context) {
        return replayTracePrefix(context, program, prefix);
      });
}

Json replayEnvelope(BufferTraceProductionFreshAdapter& adapter,
                    const ReplayReceipt& receipt, std::uint64_t repetition)
{
  return {{"schema_version", "buffer-replay-probe/1.0.0"},
          {"functional", replayReceiptToJson(receipt)},
          {"source_isolation", sourceIsolation(adapter.sourceIsolationReceipt())},
          {"derived_timing_freshness",
           derivedTimingFreshness(adapter.derivedTimingFreshnessReceipt())},
          {"run", {{"child_pid", static_cast<std::uint64_t>(::getpid())},
                    {"loader_invocation_count", adapter.loaderInvocationCount()},
                    {"guarded_work_invocation_count",
                     adapter.guardedWorkInvocationCount()},
                    {"one_shot_state", stateToken(adapter.oneShotState())},
                    {"prefix_batches", receipt.prefix_batches},
                    {"repetition", repetition}}}};
}

void replayMode(const Options& options)
{
  requireOptions(options,
                 {"--trace", "--prefix", "--repetition", "--output"});
  const BufferTraceArtifact parsed
      = requireParsed<BufferTraceArtifactJsonParseResult, BufferTraceArtifact>(
          parseBufferTraceArtifactJson(readText(options.at("--trace"))),
          "trace artifact");
  const std::uint64_t prefix = parseU64(options.at("--prefix"), "prefix");
  const std::uint64_t repetition
      = parseU64(options.at("--repetition"), "repetition");
  BufferTraceProductionFreshAdapter adapter;
  ReplayReceipt receipt
      = runReplay(adapter, parsed.replay, static_cast<std::size_t>(prefix));
  writeText(options.at("--output"),
            replayEnvelope(adapter, receipt, repetition).dump(2));
}

void templateMode(const Options& options, bool synthetic)
{
  requireOptions(options,
                 {"--tech-lef", "--cell-lef", "--liberty", "--verilog",
                  "--sdc", "--top", "--output"});
  const ReplayHeader header{
      .workload_kind = synthetic ? SemanticWorkloadKind::kSynthetic
                                 : SemanticWorkloadKind::kRealDesign,
      .profile_id = ProfileId::kEngineeringSmoke,
      .comparability_status = ComparabilityStatus::kNonComparable,
      .comparability_reason
      = ComparabilityReason::kEngineeringRcNoPhysical};
  ReplayPayload payload;
  if (synthetic) {
    const auto recipe = [&](std::string_view option) {
      const fs::path path = options.at(std::string(option));
      return std::pair{path.filename().string(), readText(path)};
    };
    auto [tech_name, tech_text] = recipe("--tech-lef");
    auto [cell_name, cell_text] = recipe("--cell-lef");
    auto [lib_name, lib_text] = recipe("--liberty");
    auto [verilog_name, verilog_text] = recipe("--verilog");
    auto [sdc_name, sdc_text] = recipe("--sdc");
    payload = SyntheticReplayPayload{
        .source_recipe
        = {{std::move(tech_name), std::move(tech_text)},
           {std::move(cell_name), std::move(cell_text)},
           {std::move(lib_name), std::move(lib_text)},
           {std::move(verilog_name), std::move(verilog_text)},
           {std::move(sdc_name), std::move(sdc_text)},
           options.at("--top")},
        .batches = {}};
  } else {
    payload = RealReplayPayload{
        .artifacts
        = {{options.at("--tech-lef")}, {options.at("--cell-lef")},
           {options.at("--liberty")}, {options.at("--verilog")},
           {options.at("--sdc")}, options.at("--top")},
        .baseline = {},
        .batches = {}};
  }
  writeText(options.at("--output"),
            replayTemplateToJson({header, std::move(payload)}));
}

void generateMode(const Options& options)
{
  requireOptions(options, {"--template", "--case-id", "--seed",
                           "--trace-output", "--output"});
  ReplayProgram replay_template
      = requireParsed<ReplayProgramJsonParseResult, ReplayProgram>(
          parseReplayTemplateJson(readText(options.at("--template"))),
          "replay template");
  const std::uint64_t seed = parseU64(options.at("--seed"), "seed");
  const ProfileId profile_id = replay_template.header.profile_id;
  BufferTraceProductionFreshAdapter adapter;
  BufferTraceGenerationResult generated = BufferFixedTraceGenerator::generate(
      adapter,
      {.replay_template = std::move(replay_template),
       .profile = frozenSelectionProfile(profile_id),
       .case_id = options.at("--case-id"),
       .seed = seed});
  test_support::requireProductionGenerationGain(generated);
  writeText(options.at("--trace-output"),
            bufferTraceArtifactToJson(generated.artifact));
  writeText(options.at("--output"),
            replayEnvelope(adapter, generated.functional_receipt, 0).dump(2));
}

void firstEmptyMode(const Options& options)
{
  requireOptions(options,
                 {"--tech-lef", "--cell-lef", "--liberty", "--verilog",
                  "--sdc", "--top", "--output"});
  const auto recipe = [&](std::string_view option) {
    const fs::path path = options.at(std::string(option));
    return std::pair{path.filename().string(), readText(path)};
  };
  auto [tech_name, tech_text] = recipe("--tech-lef");
  auto [cell_name, cell_text] = recipe("--cell-lef");
  auto [lib_name, lib_text] = recipe("--liberty");
  auto [verilog_name, verilog_text] = recipe("--verilog");
  auto [sdc_name, sdc_text] = recipe("--sdc");
  ReplayProgram replay_template{
      .header
      = {.workload_kind = SemanticWorkloadKind::kSynthetic,
         .profile_id = ProfileId::kEngineeringSmoke,
         .comparability_status = ComparabilityStatus::kNonComparable,
         .comparability_reason
         = ComparabilityReason::kEngineeringRcNoPhysical},
      .workload
      = SyntheticReplayPayload{
          .source_recipe
          = {{std::move(tech_name), std::move(tech_text)},
             {std::move(cell_name), std::move(cell_text)},
             {std::move(lib_name), std::move(lib_text)},
             {std::move(verilog_name), std::move(verilog_text)},
             {std::move(sdc_name), std::move(sdc_text)}, options.at("--top")},
          .batches = {}}};
  BufferTraceProductionFreshAdapter adapter(true);
  std::string error_what;
  try {
    static_cast<void>(BufferFixedTraceGenerator::generate(
        adapter,
        {.replay_template = std::move(replay_template),
         .profile = frozenSelectionProfile(ProfileId::kEngineeringSmoke),
         .case_id = "stage4-first-empty",
         .seed = 42}));
    fail("first empty generation unexpectedly returned an artifact");
  } catch (const BufferGenerationError& error) {
    constexpr std::string_view kExpected
        = "invalid_generated_artifact: empty_accepted_batch: no replayable "
          "batch was generated";
    if (error.code() != BufferGenerationFailureCode::kInvalidArtifact
        || error.what() != kExpected) {
      fail("first empty generation returned the wrong typed failure");
    }
    error_what = error.what();
  }
  const Json isolation = sourceIsolation(adapter.sourceIsolationReceipt());
  const Json freshness
      = derivedTimingFreshness(adapter.derivedTimingFreshnessReceipt());
  if (!adapter.firstEmptyGenerationZeroExact()
      || adapter.loaderInvocationCount() != 1
      || adapter.guardedWorkInvocationCount() != 1
      || adapter.oneShotState() != OneShotProcessState::kFinished) {
    fail("first empty failure touched generation, loader, or guarded state");
  }
  writeText(
      options.at("--output"),
      Json{{"schema_version", "buffer-first-empty-receipt/1.0.0"},
           {"error_code", "invalid_generated_artifact"},
           {"error_what", error_what},
           {"generation_zero_exact", true},
           {"native_database_installed", false},
           {"negative_gain_selection_exact", true},
           {"source_isolation", isolation},
           {"derived_timing_freshness", freshness},
           {"loader_invocation_count", adapter.loaderInvocationCount()},
           {"guarded_work_invocation_count",
            adapter.guardedWorkInvocationCount()},
           {"one_shot_state", stateToken(adapter.oneShotState())}}
          .dump(2));
}

void preloaderRejectionMode(const Options& options)
{
  requireOptions(options,
                 {"--tech-lef", "--cell-lef", "--liberty", "--verilog",
                  "--sdc", "--top", "--output"});
  ReplayProgram replay_template{
      .header
      = {.workload_kind = SemanticWorkloadKind::kRealDesign,
         .profile_id = ProfileId::kEngineeringSmoke,
         .comparability_status = ComparabilityStatus::kNonComparable,
         .comparability_reason
         = ComparabilityReason::kEngineeringRcNoPhysical},
      .workload
      = RealReplayPayload{
          .artifacts
          = {{options.at("--tech-lef")}, {options.at("--cell-lef")},
             {options.at("--liberty")}, {options.at("--verilog")},
             {options.at("--sdc")}, options.at("--top")},
          .baseline = {},
          .batches = {}}};
  BufferTraceProductionFreshAdapter adapter;
  bool work_entered = false;
  try {
    static_cast<void>(BufferFixedTraceGenerator::generate(
        adapter,
        BufferTraceGenerationRequest{
            .replay_template = std::move(replay_template),
            .profile = frozenSelectionProfile(ProfileId::kEngineeringSmoke),
            .case_id = "preloader_rejection",
            .seed = 0}));
    fail("malformed source set unexpectedly entered the production loader");
  } catch (const FreshBaselineError& error) {
    if (error.code()
            != FreshBaselineFailureCode::kArtifactMaterializationRejected
        || std::string_view(token(error.code()))
               != "artifact_materialization_rejected") {
      fail("preloader rejection returned the wrong typed failure");
    }
  }
  if (work_entered || adapter.loaderInvocationCount() != 0
      || adapter.guardedWorkInvocationCount() != 0
      || adapter.oneShotState() != OneShotProcessState::kFinished) {
    fail("artifact rejection occurred after loader or guarded work entry");
  }
  writeText(
      options.at("--output"),
      Json{{"schema_version", "buffer-preloader-rejection/1.0.0"},
           {"error_code", "artifact_materialization_rejected"},
           {"work_entered", work_entered},
           {"loader_invocation_count", adapter.loaderInvocationCount()},
           {"guarded_work_invocation_count",
            adapter.guardedWorkInvocationCount()},
           {"one_shot_state", stateToken(adapter.oneShotState())}}
          .dump(2));
}

void doubleInvocationMode(const Options& options)
{
  requireOptions(options, {"--trace", "--output"});
  const BufferTraceArtifact parsed
      = requireParsed<BufferTraceArtifactJsonParseResult, BufferTraceArtifact>(
          parseBufferTraceArtifactJson(readText(options.at("--trace"))),
          "trace artifact");
  BufferTraceProductionFreshAdapter adapter;
  ReplayReceipt first = runReplay(
      adapter, parsed.replay,
      std::visit([](const auto& payload) { return payload.batches.size(); },
                 parsed.replay.workload));
  bool second_work_entered = false;
  std::string error_what;
  try {
    static_cast<void>(adapter.withFreshBaseline(
        parsed.replay, [&](BufferReplayContext& context) {
          second_work_entered = true;
          return replayTracePrefix(context, parsed.replay, 1);
        }));
    fail("second same-process fresh entry was accepted");
  } catch (const FreshBaselineError& error) {
    if (error.code() != FreshBaselineFailureCode::kFreshProcessReuseRejected
        || std::string_view(token(error.code()))
               != "fresh_process_reuse_rejected") {
      fail("second fresh entry returned the wrong typed error");
    }
    error_what = error.what();
  }
  if (second_work_entered || adapter.loaderInvocationCount() != 1
      || adapter.guardedWorkInvocationCount() != 1
      || adapter.oneShotState() != OneShotProcessState::kFinished) {
    fail("typed reuse rejection occurred after entering loader/work");
  }
  writeText(options.at("--output"),
            Json{{"schema_version", "buffer-one-shot-self-test/1.0.0"},
                 {"first_functional", replayReceiptToJson(first)},
                 {"source_isolation", sourceIsolation(adapter.sourceIsolationReceipt())},
                 {"derived_timing_freshness",
                  derivedTimingFreshness(adapter.derivedTimingFreshnessReceipt())},
                 {"second_error_code", "fresh_process_reuse_rejected"},
                 {"second_error_what", error_what},
                 {"second_work_entered", second_work_entered},
                 {"loader_invocation_count", adapter.loaderInvocationCount()},
                 {"guarded_work_invocation_count",
                  adapter.guardedWorkInvocationCount()},
                 {"one_shot_state", stateToken(adapter.oneShotState())}}
                .dump(2));
}

}  // namespace
}  // namespace ista::buffer_trace::test

int main(int argc, char* argv[])
{
  try {
    std::string mode;
    const auto options
        = ista::buffer_trace::test::parseOptions(argc, argv, mode);
    if (mode == "replay") {
      ista::buffer_trace::test::replayMode(options);
    } else if (mode == "make-synthetic-template") {
      ista::buffer_trace::test::templateMode(options, true);
    } else if (mode == "make-real-template") {
      ista::buffer_trace::test::templateMode(options, false);
    } else if (mode == "generate") {
      ista::buffer_trace::test::generateMode(options);
    } else if (mode == "self-test-double-invocation") {
      ista::buffer_trace::test::doubleInvocationMode(options);
    } else if (mode == "expect-first-empty") {
      ista::buffer_trace::test::firstEmptyMode(options);
    } else if (mode == "expect-preloader-reject") {
      ista::buffer_trace::test::preloaderRejectionMode(options);
    } else {
      throw std::runtime_error("unknown probe mode: " + mode);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
