#include "IncreGpuStaBuildConfig.hpp"
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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <any>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "CsrFifoLevelization.hpp"
#include "CsrGraphEncoding.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "DataManager.hpp"
#include "IncrementalStaSession.hpp"
#include "STAInterface.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingSemanticKey.hpp"
#include "iSTA/source/module/timing_csr_snapshot/CsrSnapshotObservation.hpp"
#include "iSTA/source/module/timing_csr_snapshot/CsrSnapshotObservationTransition.hpp"
#include "iSTA/source/module/timing_csr_snapshot/CsrSnapshotObservationValidator.hpp"
#include "idm.h"
#include "json.hpp"

namespace ista {

class StaCsrSessionProbeAccess
{
 public:
  static void inspect(std::uint64_t token, const std::function<void(const Database&)>& callback)
  {
    STAInterface::getInst().inspectSessionDatabaseForTesting(token, callback);
  }
};

}  // namespace ista

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Options
{
  fs::path tech_lef;
  fs::path cell_lef;
  fs::path liberty;
  fs::path verilog;
  fs::path sdc;
  std::string top;
  std::string case_name;
  std::string expected_clock;
  std::uint64_t repeat_index{0};
  std::string run_nonce;
  fs::path run_dir;
  fs::path result_temp;
  bool session_mode{false};
};

struct Observation
{
  std::vector<std::uint8_t> direct_projection;
  std::vector<std::uint8_t> decoded_projection;
  std::vector<std::uint8_t> native_graph_oracle;
  std::vector<std::uint8_t> independent_fifo_oracle;
  std::size_t node_count{0};
  std::size_t arc_count{0};
};

struct SessionOracle
{
  ista::csr::CsrSnapshotIdentity source_identity;
  std::uint64_t node_count{0};
  std::uint64_t edge_count{0};
  std::uint64_t level_count{0};
  std::vector<std::uint8_t> projection_bytes;
  std::vector<std::uint8_t> fifo_bytes;
};

struct SessionRun
{
  std::uint64_t session_token{0};
  ista::csr::CsrSnapshotObservation active;
  ista::csr::CsrSnapshotObservation released;
  SessionOracle oracle;
};

bool isLowerHex(std::string_view value, std::size_t size)
{
  if (value.size() != size)
    return false;
  for (char character : value) {
    if ((character < '0' || character > '9') && (character < 'a' || character > 'f'))
      return false;
  }
  return true;
}

std::uint64_t parseIndex(const std::string& value)
{
  if (value.empty())
    throw std::runtime_error("repeat index is empty");
  std::uint64_t result = 0;
  for (char character : value) {
    if (character < '0' || character > '9')
      throw std::runtime_error("repeat index must contain decimal digits only");
    std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      throw std::runtime_error("repeat index overflows U64");
    result = result * 10 + digit;
  }
  return result;
}

Options parseOptions(int argc, char* argv[])
{
  if (argc < 2 || argc % 2 == 0)
    throw std::runtime_error("arguments must be exact --option value pairs");
  std::map<std::string, std::string> values;
  for (int index = 1; index < argc; index += 2) {
    std::string name = argv[index];
    if (!name.starts_with("--") || index + 1 >= argc)
      throw std::runtime_error("arguments must be exact --option value pairs");
    if (!values.emplace(name, argv[index + 1]).second)
      throw std::runtime_error("duplicate option: " + name);
  }
  const std::vector<std::string> required = {
      "--tech-lef",       "--cell-lef",     "--liberty",      "--verilog", "--sdc",     "--top",
      "--case-name",      "--expected-clock", "--repeat-index", "--run-nonce", "--run-dir", "--result-temp",
  };
  for (const std::string& name : required) {
    if (!values.contains(name) || values.at(name).empty())
      throw std::runtime_error("missing required option: " + name);
  }
  const auto probe_mode = values.find("--probe-mode");
  if (values.size() != required.size() + static_cast<std::size_t>(probe_mode != values.end()))
    throw std::runtime_error("unknown command-line option");
  if (probe_mode != values.end() && probe_mode->second != "session")
    throw std::runtime_error("probe mode must be session");
  return {
      values.at("--tech-lef"),
      values.at("--cell-lef"),
      values.at("--liberty"),
      values.at("--verilog"),
      values.at("--sdc"),
      values.at("--top"),
      values.at("--case-name"),
      values.at("--expected-clock"),
      parseIndex(values.at("--repeat-index")),
      values.at("--run-nonce"),
      values.at("--run-dir"),
      values.at("--result-temp"),
      probe_mode != values.end(),
  };
}

fs::path canonicalExisting(const fs::path& path, std::string_view label)
{
  if (!path.is_absolute() || !fs::is_regular_file(path))
    throw std::runtime_error(std::string(label) + " must be an absolute regular file");
  return fs::canonical(path);
}

void validateOptions(Options& options)
{
  options.tech_lef = canonicalExisting(options.tech_lef, "technology LEF");
  options.cell_lef = canonicalExisting(options.cell_lef, "cell LEF");
  options.liberty = canonicalExisting(options.liberty, "Liberty");
  options.verilog = canonicalExisting(options.verilog, "Verilog");
  options.sdc = canonicalExisting(options.sdc, "SDC");
  if (options.case_name != "chain" && options.case_name != "diamond")
    throw std::runtime_error("case name must be chain or diamond");
  if (options.top.empty() || options.expected_clock.empty())
    throw std::runtime_error("top and expected clock must be non-empty");
  if (!isLowerHex(options.run_nonce, 32))
    throw std::runtime_error("run nonce must be 32 lowercase hex characters");
  if (!options.run_dir.is_absolute() || !fs::is_directory(options.run_dir))
    throw std::runtime_error("run directory must be an existing absolute directory");
  options.run_dir = fs::canonical(options.run_dir);
  if (!options.result_temp.is_absolute())
    throw std::runtime_error("result temporary path must be absolute");
  fs::path normalized_temp = options.result_temp.lexically_normal();
  const std::string temporary_name = (options.session_mode ? ".csr-session-result-" : ".projection-result-") + options.run_nonce + ".tmp";
  if (normalized_temp.parent_path() != options.run_dir || normalized_temp.filename() != temporary_name)
    throw std::runtime_error("result temporary path is not the nonce-bound child of run directory");
  options.result_temp = normalized_temp;
  std::error_code error;
  if (fs::symlink_status(options.result_temp, error).type() != fs::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
    throw std::runtime_error("result temporary path already exists or cannot be inspected");
  fs::path forbidden_final = options.run_dir / (options.session_mode ? "csr-session-result.json" : "projection-result.json");
  error.clear();
  if (fs::symlink_status(forbidden_final, error).type() != fs::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
    throw std::runtime_error("controller-owned final path already exists or cannot be inspected");
}

void loadDesign(const Options& options)
{
  if (!dmInst->readLef(std::vector<std::string>{options.tech_lef.string()}, true))
    throw std::runtime_error("failed to read technology LEF");
  if (!dmInst->readLef(std::vector<std::string>{options.cell_lef.string()}))
    throw std::runtime_error("failed to read cell LEF");
  dmInst->get_config().set_sdc_path(options.sdc.string());
  if (!dmInst->readVerilog(options.verilog.string(), options.top))
    throw std::runtime_error("failed to read Verilog");
  if (!dmInst->readLib(std::vector<std::string>{options.liberty.string()}))
    throw std::runtime_error("failed to read Liberty");
}

void requireFullStaState(const Options& options, const ista::Database& database)
{
  if (!database.get_timing_constraint().get_clock_map().contains(options.expected_clock))
    throw std::runtime_error("expected clock was not created");
  if (database.get_pin_map().empty() || database.get_arc_list().empty())
    throw std::runtime_error("full STA produced an empty native graph");
  if (database.get_end_point_list().empty() || database.get_timing_order_list().empty())
    throw std::runtime_error("full STA produced no endpoints or timing order");
}

const ista::csr::StaAnalysisContext& probeAnalysisContext()
{
  static const ista::csr::StaAnalysisContext context{"slow", "25", "__no_rc_corner__"};
  return context;
}

std::map<std::string, std::any> makeConfig(const Options& options)
{
  return {{"-temp_directory_path", options.run_dir.string()},
          {"-thread_number", int32_t{1}},
          {"-output_timing_reports", int32_t{1}},
          {"-output_timing_features", int32_t{1}},
          {"-timing_path_limit", int32_t{20}},
          {"-max_paths", int32_t{20}},
          {"-nworst", int32_t{20}},
          {"-timing_corner", std::string{}}};
}

void inspectDatabase(std::uint64_t token, const std::function<void(const ista::Database&)>& callback)
{
  ista::StaCsrSessionProbeAccess::inspect(token, callback);
}

std::uint64_t requireSessionToken(const ista::IncrementalStaSession& session)
{
  const std::optional<std::uint64_t> token = session.sessionToken();
  if (!token || *token == 0)
    throw std::runtime_error("initialized STA session has no nonzero token");
  return *token;
}

template <typename Value, typename Failure>
const Value& requireValue(const std::variant<Value, Failure>& result, std::string_view label)
{
  if (const auto* value = std::get_if<Value>(&result))
    return *value;
  throw std::runtime_error(std::string(label) + " returned a typed failure");
}

Observation observeDatabase(const ista::Database& database)
{
  using namespace ista::csr;
  using namespace ista::csr::test;
  NativeGraphProjection direct_before = captureDirectNativeGraphProjection(database);
  std::vector<std::uint8_t> before_bytes = canonicalIndependentNativeGraphProjectionBytes(direct_before);
  CsrGraphExportResult exported = exportCsrGraphSnapshot(database);
  const CsrGraphSnapshot& snapshot = requireValue(exported, "CSR graph export");
  NativeGraphProjectionResult decoded_result = decodeNativeGraphProjection(snapshot);
  const NativeGraphProjection& decoded = requireValue(decoded_result, "native graph decode");
  NativeGraphProjection direct_after = captureDirectNativeGraphProjection(database);
  if (direct_before != direct_after)
    throw std::runtime_error("database projection changed during export/decode");
  if (direct_before != decoded)
    throw std::runtime_error("direct and decoded native graph projections differ");

  CsrNativeGraphOracle native_oracle = readNativeGraphOracle(database);
  IndependentFifoOracle fifo = runIndependentFifoOracle(database);
  if (fifo.has_active_cycle)
    throw std::runtime_error("hermetic DAG fixture unexpectedly contains an active cycle");
  if (native_oracle != fifo.oracle)
    throw std::runtime_error("native and independent FIFO graph oracles differ");

  Observation observation;
  observation.direct_projection = std::move(before_bytes);
  observation.decoded_projection = canonicalNativeGraphProjectionBytes(decoded);
  observation.native_graph_oracle = canonicalNativeGraphOracleBytes(native_oracle);
  observation.independent_fifo_oracle = canonicalIndependentNativeGraphOracleBytes(fifo.oracle);
  observation.node_count = decoded.nodes.size();
  observation.arc_count = decoded.arcs.size();
  if (observation.direct_projection != observation.decoded_projection)
    throw std::runtime_error("independent and production projection encodings differ");
  if (observation.native_graph_oracle != observation.independent_fifo_oracle)
    throw std::runtime_error("independent and production oracle encodings differ");
  if (observation.node_count == 0 || observation.arc_count == 0)
    throw std::runtime_error("decoded graph shape is empty");
  return observation;
}

ista::csr::CsrNativeGraphOracle makeReplayOracle(const ista::csr::CsrReferenceInput& input, const ista::csr::CsrFifoLevelization& replay)
{
  using namespace ista::csr;
  CsrNativeGraphOracle oracle;
  oracle.native_timing_order.reserve(replay.level_nodes.size());
  for (NodeId node : replay.level_nodes)
    oracle.native_timing_order.push_back(input.nodes.at(node.value()).key);
  oracle.native_node_levels.reserve(input.nodes.size());
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    const std::uint64_t level = replay.node_to_level.at(index);
    if (level > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
      throw std::runtime_error("CSR replay level exceeds I32");
    oracle.native_node_levels.push_back({input.nodes[index].key, static_cast<std::int32_t>(level)});
  }
  std::sort(oracle.native_node_levels.begin(), oracle.native_node_levels.end(),
            [](const auto& lhs, const auto& rhs) { return canonicalNodeKeyBytes(lhs.node_key) < canonicalNodeKeyBytes(rhs.node_key); });
  return oracle;
}

SessionOracle observeSessionDatabase(const ista::Database& database, std::uint64_t token)
{
  using namespace ista::csr;
  using namespace ista::csr::test;
  NativeGraphProjection direct_before = captureDirectNativeGraphProjection(database);
  IndependentFifoOracle independent = runIndependentFifoOracle(database);
  if (independent.has_active_cycle)
    throw std::runtime_error("hermetic DAG fixture unexpectedly contains an active cycle");

  CsrReferenceInputExportResult exported = exportCsrReferenceInputCandidate(database, probeAnalysisContext(), token);
  const CsrReferenceInput& input = requireValue(exported, "CSR reference input export");
  CsrFifoLevelization replay = replayFifoLevelization(input);
  NativeGraphProjection direct_after = captureDirectNativeGraphProjection(database);
  if (direct_before != direct_after)
    throw std::runtime_error("database projection changed during session oracle capture");
  if (!replay.complete || replay.level_offsets.size() <= 1)
    throw std::runtime_error("CSR FIFO replay is incomplete or empty");
  if (direct_before.nodes.size() != input.nodes.size() || direct_before.arcs.size() != input.arcs.size())
    throw std::runtime_error("direct projection and reference input counts differ");
  for (std::size_t index = 0; index < input.nodes.size(); ++index)
    if (direct_before.nodes[index].key != input.nodes[index].key)
      throw std::runtime_error("direct projection and reference input node keys differ");
  for (std::size_t index = 0; index < input.arcs.size(); ++index)
    if (direct_before.arcs[index].key != input.arcs[index].key)
      throw std::runtime_error("direct projection and reference input arc keys differ");

  const CsrNativeGraphOracle replay_oracle = makeReplayOracle(input, replay);
  if (replay_oracle != independent.oracle)
    throw std::runtime_error("independent native FIFO and reference-input replay differ");

  SessionOracle oracle;
  oracle.source_identity = input.source_identity;
  oracle.node_count = static_cast<std::uint64_t>(input.nodes.size());
  oracle.edge_count = static_cast<std::uint64_t>(input.arcs.size());
  oracle.level_count = static_cast<std::uint64_t>(replay.level_offsets.size() - 1);
  oracle.projection_bytes = canonicalIndependentNativeGraphProjectionBytes(direct_before);
  oracle.fifo_bytes = canonicalIndependentNativeGraphOracleBytes(independent.oracle);
  if (oracle.node_count == 0 || oracle.edge_count == 0 || oracle.level_count == 0)
    throw std::runtime_error("session oracle produced an empty graph");
  return oracle;
}

void requireObservation(const ista::csr::CsrSnapshotObservation& observation, ista::csr::CsrSnapshotState state, const SessionOracle& oracle,
                        std::string_view label)
{
  using namespace ista::csr;
  const auto violations = validateSnapshotObservation(observation);
  if (!violations.empty())
    throw std::runtime_error(std::string(label) + " violates " + violations.front().field + ": " + violations.front().message);
  if (observation.state != state)
    throw std::runtime_error(std::string(label) + " has the wrong state");
  if (observation.identity != oracle.source_identity || observation.identity.session_token == 0 || observation.identity.graph_generation != 0)
    throw std::runtime_error(std::string(label) + " identity differs from the direct exporter");
  if (observation.counts.nodes != oracle.node_count || observation.counts.arcs != oracle.edge_count || observation.counts.levels != oracle.level_count)
    throw std::runtime_error(std::string(label) + " counts differ from the direct oracle");
  if (observation.last_rejection || observation.last_invalidation)
    throw std::runtime_error(std::string(label) + " initial capture unexpectedly contains an event");
  if (observation.timing_ms.reference_ms || observation.timing_ms.scatter_ms)
    throw std::runtime_error(std::string(label) + " initial capture unexpectedly contains operation timing");
}

void requireReleaseTransition(const ista::csr::CsrSnapshotObservation& active, const ista::csr::CsrSnapshotObservation& released)
{
  using namespace ista::csr;
  const CsrOptionalObservation before{active};
  const CsrOptionalObservation after{released};
  const auto violations = validateSnapshotObservationTransition(before, after, ReleaseAction{});
  if (!violations.empty())
    throw std::runtime_error("released observation transition violates " + violations.front().field + ": " + violations.front().message);
}

std::optional<Observation> runFullProbe(const Options& options, bool capture_projection)
{
  using namespace ista;
  IncrementalStaSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError});
  throwOnStaFailure(session.initialize(probeAnalysisContext(), makeConfig(options)), session.request());
  const std::uint64_t token = requireSessionToken(session);
  const StaExecutionRecord record = requireStaExecutionRecord(session.runFullBaseline(), session.request());
  if (record.execution_kind != StaExecutionKind::kFullUpdate || record.fallback_reason || record.fallback_to_full)
    throw std::runtime_error("full STA execution record mismatch");
  if (session.snapshotObservation())
    throw std::runtime_error("cpu/full session published a CSR observation");
  std::optional<Observation> observation;
  inspectDatabase(token, [&](const Database& database) {
    requireFullStaState(options, database);
    if (capture_projection)
      observation = observeDatabase(database);
  });
  throwOnStaFailure(session.close(), session.request());
  if (session.snapshotObservation())
    throw std::runtime_error("closed cpu/full session retained a CSR observation");
  return observation;
}

SessionRun runIncrementalProbe(const Options& options)
{
  using namespace ista;
  using namespace ista::csr;
  if (!dmInst->readLib(std::vector<std::string>{options.liberty.string()}))
    throw std::runtime_error("failed to refresh Liberty for sequential session");
  IncrementalStaSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError});
  throwOnStaFailure(session.initialize(probeAnalysisContext(), makeConfig(options)), session.request());
  const std::uint64_t token = requireSessionToken(session);
  const StaExecutionRecord record = requireStaExecutionRecord(session.runFullBaseline(), session.request());
  if (record.execution_kind != StaExecutionKind::kBaselineOnly || record.fallback_to_full
      || record.fallback_reason != StaFallbackReason::kNotApplicableNoMutation)
    throw std::runtime_error("incremental STA baseline execution record mismatch");

  std::unique_ptr<CsrSnapshotObservation> active = session.snapshotObservation();
  if (!active)
    throw std::runtime_error("incremental session did not publish an active observation");
  SessionOracle oracle;
  inspectDatabase(token, [&](const Database& database) {
    requireFullStaState(options, database);
    oracle = observeSessionDatabase(database, token);
  });
  requireObservation(*active, CsrSnapshotState::kReady, oracle, "active observation");

  throwOnStaFailure(session.close(), session.request());
  std::unique_ptr<CsrSnapshotObservation> released = session.snapshotObservation();
  if (!released)
    throw std::runtime_error("incremental session did not retain a released observation");
  requireObservation(*released, CsrSnapshotState::kReleased, oracle, "released observation");
  requireReleaseTransition(*active, *released);
  if (session.sessionToken() != std::optional<std::uint64_t>{token})
    throw std::runtime_error("incremental session token changed after close");
  return {token, std::move(*active), std::move(*released), std::move(oracle)};
}

std::string toHex(std::span<const std::uint8_t> bytes)
{
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

Json makeResult(const Options& options, const Observation& observation)
{
  return {
      {"schema", "ista-csr-projection-probe/1.0.0"},
      {"run_nonce", options.run_nonce},
      {"case", options.case_name},
      {"top", options.top},
      {"expected_clock", options.expected_clock},
      {"repeat_index", options.repeat_index},
      {"node_count", observation.node_count},
      {"arc_count", observation.arc_count},
      {"direct_projection", toHex(observation.direct_projection)},
      {"decoded_projection", toHex(observation.decoded_projection)},
      {"native_graph_oracle", toHex(observation.native_graph_oracle)},
      {"independent_fifo_oracle", toHex(observation.independent_fifo_oracle)},
      {"complete", true},
  };
}

Json identityJson(const ista::csr::CsrSnapshotIdentity& identity)
{
  return {{"fingerprint_schema_version", identity.fingerprint_schema_version},
          {"session_token", identity.session_token},
          {"graph_generation", identity.graph_generation},
          {"topology_fingerprint", identity.topology_fingerprint},
          {"clock_constraint_fingerprint", identity.clock_constraint_fingerprint},
          {"library_fingerprint", identity.library_fingerprint},
          {"corner_fingerprint", identity.corner_fingerprint},
          {"parasitic_fingerprint", identity.parasitic_fingerprint},
          {"timing_input_fingerprint", identity.timing_input_fingerprint}};
}

Json extentJson(const ista::csr::CsrBufferExtent& extent)
{
  return {{"logical_size", extent.logical_size}, {"capacity", extent.capacity}};
}

Json optionalDoubleJson(const std::optional<double>& value)
{
  return value ? Json(*value) : Json(nullptr);
}

Json observationJson(const ista::csr::CsrSnapshotObservation& observation)
{
  Json reasons = Json::array();
  for (const auto reason : observation.support.unsupported_reasons)
    reasons.push_back(std::string(ista::csr::token(reason)));
  return {{"schema_version", observation.schema_version},
          {"state", std::string(ista::csr::token(observation.state))},
          {"identity", identityJson(observation.identity)},
          {"counts", {{"nodes", observation.counts.nodes}, {"arcs", observation.counts.arcs}, {"levels", observation.counts.levels}}},
          {"buffers",
           {{"node_keys", extentJson(observation.buffers.node_keys)},
            {"arc_keys", extentJson(observation.buffers.arc_keys)},
            {"out_row_offsets", extentJson(observation.buffers.out_row_offsets)},
            {"out_arc_ids", extentJson(observation.buffers.out_arc_ids)},
            {"in_row_offsets", extentJson(observation.buffers.in_row_offsets)},
            {"in_arc_ids", extentJson(observation.buffers.in_arc_ids)},
            {"level_offsets", extentJson(observation.buffers.level_offsets)},
            {"level_nodes", extentJson(observation.buffers.level_nodes)},
            {"node_to_level", extentJson(observation.buffers.node_to_level)}}},
          {"capacity", {{"last_action", std::string(ista::csr::token(observation.capacity.last_action))}, {"event_count", observation.capacity.event_count}}},
          {"support", {{"reference_supported", observation.support.reference_supported}, {"unsupported_reasons", std::move(reasons)}}},
          {"timing_ms",
           {{"export", observation.timing_ms.export_ms},
            {"reference", optionalDoubleJson(observation.timing_ms.reference_ms)},
            {"scatter", optionalDoubleJson(observation.timing_ms.scatter_ms)}}},
          {"host_bytes", {{"logical", observation.host_bytes.logical}, {"capacity", observation.host_bytes.capacity}}},
          {"last_rejection", nullptr},
          {"last_invalidation", nullptr}};
}

Json sessionJson(const SessionRun& session)
{
  return {{"session_token", session.session_token},
          {"active", observationJson(session.active)},
          {"released", observationJson(session.released)},
          {"oracle",
           {{"source_identity", identityJson(session.oracle.source_identity)},
            {"node_count", session.oracle.node_count},
            {"edge_count", session.oracle.edge_count},
            {"level_count", session.oracle.level_count},
            {"projection_wire_hex", toHex(session.oracle.projection_bytes)},
            {"fifo_wire_hex", toHex(session.oracle.fifo_bytes)}}}};
}

bool sameContentIdentity(const ista::csr::CsrSnapshotIdentity& lhs, const ista::csr::CsrSnapshotIdentity& rhs)
{
  return lhs.topology_fingerprint == rhs.topology_fingerprint && lhs.clock_constraint_fingerprint == rhs.clock_constraint_fingerprint
         && lhs.library_fingerprint == rhs.library_fingerprint && lhs.corner_fingerprint == rhs.corner_fingerprint
         && lhs.parasitic_fingerprint == rhs.parasitic_fingerprint && lhs.timing_input_fingerprint == rhs.timing_input_fingerprint;
}

Json makeSessionResult(const Options& options, const SessionRun& first, const SessionRun& second)
{
  if (first.session_token == second.session_token || !sameContentIdentity(first.active.identity, second.active.identity)
      || first.active.counts != second.active.counts || first.oracle.projection_bytes != second.oracle.projection_bytes
      || first.oracle.fifo_bytes != second.oracle.fifo_bytes)
    throw std::runtime_error("same-input sequential session identity, counts, or oracle bytes differ");
  return {{"schema", "ista-csr-session-probe/1.0.0"},
          {"run_nonce", options.run_nonce},
          {"case", options.case_name},
          {"repeat_index", options.repeat_index},
          {"full_observation", nullptr},
          {"sessions", Json::array({sessionJson(first), sessionJson(second)})},
          {"complete", true}};
}

void writeExclusive(const fs::path& path, const std::string& payload)
{
  int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (descriptor < 0)
    throw std::runtime_error("failed to exclusively create result temporary: " + std::string(std::strerror(errno)));
  bool created = true;
  try {
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_uid != ::geteuid())
      throw std::runtime_error("exclusive result temporary is not one current-user regular file");
    std::size_t written = 0;
    while (written < payload.size()) {
      ssize_t count = ::write(descriptor, payload.data() + written, payload.size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        throw std::runtime_error("failed to write result temporary");
      written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0)
      throw std::runtime_error("failed to synchronize result temporary");
    if (::close(descriptor) != 0)
      throw std::runtime_error("failed to close result temporary");
    descriptor = -1;
    created = false;
  } catch (...) {
    int saved_errno = errno;
    if (descriptor >= 0)
      static_cast<void>(::close(descriptor));
    if (created)
      static_cast<void>(::unlink(path.c_str()));
    errno = saved_errno;
    throw;
  }
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    if constexpr (!ista::incregpusta_debug_checks_enabled)
      throw std::runtime_error("diagnostic probe requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
    Options options = parseOptions(argc, argv);
    validateOptions(options);
    loadDesign(options);
    Json result;
    if (options.session_mode) {
      static_cast<void>(runFullProbe(options, false));
      const SessionRun first = runIncrementalProbe(options);
      const SessionRun second = runIncrementalProbe(options);
      result = makeSessionResult(options, first, second);
    } else {
      const std::optional<Observation> observation = runFullProbe(options, true);
      if (!observation)
        throw std::runtime_error("default full probe did not capture a projection");
      result = makeResult(options, *observation);
    }
    ista::STAInterface::destroyInst();
    writeExclusive(options.result_temp, result.dump(2) + "\n");
    return 0;
  } catch (const std::exception& error) {
    try {
      ista::STAInterface::destroyInst();
    } catch (const std::exception& cleanup_error) {
      std::cerr << "StaCsrProjectionProbe cleanup: " << cleanup_error.what() << '\n';
    }
    std::cerr << "StaCsrProjectionProbe: " << error.what() << '\n';
    return 1;
  }
}
