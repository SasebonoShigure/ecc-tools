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
#include <unistd.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "CsrRealDifferentialNativeReadback.hpp"
#include "CsrRealDifferentialProbeSupport.hpp"
#include "CsrRealDifferentialReferenceEncoding.hpp"
#include "CsrRealDifferentialScenario.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferencePoisonSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "CsrSnapshotObservationValidator.hpp"
#include "Database.hpp"

namespace ista::csr::real_diff {
namespace {

struct RawCheckpoint
{
  test::CsrReferenceInputRawProjection projection;
  std::vector<std::uint8_t> bytes;
  std::string sha256;
  Json json;
};

[[noreturn]] void fail(std::string_view message)
{
  throw std::runtime_error("real differential reference probe: " + std::string(message));
}

void require(bool condition, std::string_view message)
{
  if (!condition) {
    fail(message);
  }
}

std::string byteSha(const std::vector<std::uint8_t>& bytes)
{
  return sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

RawCheckpoint takeRawCheckpoint(const Database& database, std::string_view name, std::uint64_t ordinal, Json& receipts)
{
  RawCheckpoint checkpoint;
  checkpoint.projection = test::projectCsrReferenceInputRaw(database);
  checkpoint.bytes = test::canonicalIndependentReferenceInputRawBytes(checkpoint.projection);
  require(!checkpoint.bytes.empty(), "raw checkpoint wire is empty");
  checkpoint.sha256 = byteSha(checkpoint.bytes);
  checkpoint.json = {{"raw_wire_hex", bytesToHex(checkpoint.bytes)}, {"raw_sha256", checkpoint.sha256}};
  receipts.push_back({{"checkpoint", name}, {"call_ordinal", ordinal}});
  return checkpoint;
}

Json poisonToJson(const std::vector<test::ReferencePoisonCanary>& canaries)
{
  require(canaries.size() == test::kReferencePoisonFamilyTokens.size(), "poison family count differs");
  Json result = Json::array();
  for (std::size_t index = 0; index < canaries.size(); ++index) {
    const test::ReferencePoisonCanary& canary = canaries[index];
    require(canary.family == test::kReferencePoisonFamilyTokens[index], "poison family order differs");
    require(!canary.before_typed_bytes.empty() && !canary.after_typed_bytes.empty(), "poison typed evidence is empty");
    const std::string before = byteSha(canary.before_typed_bytes);
    const std::string after = byteSha(canary.after_typed_bytes);
    require(before != after, "poison family did not change");
    result.push_back({{"family", canary.family}, {"before_sha256", before}, {"after_sha256", after}});
  }
  return result;
}

bool fingerprintsMatch(const CsrCurrentFingerprints& current, const CsrSnapshotIdentity& identity)
{
  return current.topology_fingerprint == identity.topology_fingerprint && current.clock_constraint_fingerprint == identity.clock_constraint_fingerprint
         && current.library_fingerprint == identity.library_fingerprint && current.corner_fingerprint == identity.corner_fingerprint
         && current.parasitic_fingerprint == identity.parasitic_fingerprint && current.timing_input_fingerprint == identity.timing_input_fingerprint;
}

template <typename T>
bool anyLane(const NeutralLaneMatrix<T>& lanes)
{
  for (const auto& analysis : lanes) {
    for (const auto& lane : analysis) {
      if (lane) {
        return true;
      }
    }
  }
  return false;
}

template <typename T>
bool anyArcLane(const NeutralArcLaneMatrix<T>& lanes)
{
  for (const auto& analysis : lanes) {
    for (const auto& input : analysis) {
      for (const auto& lane : input) {
        if (lane) {
          return true;
        }
      }
    }
  }
  return false;
}

const NeutralNode* findNode(const NeutralWire& wire, const NodeKey& key)
{
  for (const NeutralNode& node : wire.nodes) {
    if (node.key == key) {
      return &node;
    }
  }
  return nullptr;
}

const NeutralArc* findArc(const NeutralWire& wire, const ArcKey& key)
{
  for (const NeutralArc& arc : wire.arcs) {
    if (arc.key == key) {
      return &arc;
    }
  }
  return nullptr;
}

bool hasTimingArcLane(const NeutralArc& arc)
{
  for (const NeutralArcEntry& entry : arc.timing_arcs) {
    if (anyArcLane(entry.lanes)) {
      return true;
    }
  }
  return false;
}

CanarySelection selectCanaries(const CsrReferenceInput& input, const NeutralWire& scattered)
{
  CanarySelection selection;
  bool have_arc = false;
  bool have_clock = false;
  bool have_data = false;
  bool have_endpoint = false;
  for (const ArcRecord& record : input.arcs) {
    if (record.kind != ArcKind::kCell || record.is_disabled || record.is_loop_disabled || !record.eligibility.data_arrival
        || record.native_traversal_rank >= input.arcs.size() || record.source.value() >= input.nodes.size()) {
      continue;
    }
    const NeutralArc* arc = findArc(scattered, record.key);
    const NeutralNode* source = findNode(scattered, input.nodes[record.source.value()].key);
    if (arc != nullptr && source != nullptr && !source->paths.empty() && anyArcLane(arc->graph_delays) && hasTimingArcLane(*arc)) {
      selection.delay_arc_native_rank = static_cast<std::size_t>(record.native_traversal_rank);
      selection.delay_arc_key = record.key;
      have_arc = true;
      break;
    }
  }
  for (const NeutralNode& node : scattered.nodes) {
    if (!have_clock && node.is_clock_point && node.clock_name && anyLane(node.effective_clock.arrivals) && anyLane(node.effective_clock.slews)) {
      selection.clock_node = node.key;
      have_clock = true;
    }
    if (!have_data && std::isfinite(node.arrival) && !node.paths.empty() && anyLane(node.data_slews) && node.predecessor_node && node.predecessor_arc) {
      selection.data_node = node.key;
      have_data = true;
    }
  }
  for (const NodeRecord& record : input.nodes) {
    const NeutralNode* node = findNode(scattered, record.key);
    if (record.is_endpoint && node != nullptr && std::isfinite(node->required) && std::isfinite(node->slack)) {
      selection.endpoint_node = record.key;
      have_endpoint = true;
      break;
    }
  }
  require(have_arc, "no active reachable bound cell Arc has a present guaranteed-overwrite lane");
  require(have_clock, "no named clock point has present guaranteed-overwrite state");
  require(have_data, "no reachable data point has finite present guaranteed-overwrite state");
  require(have_endpoint, "no endpoint has finite required/slack state");
  return selection;
}

void requireDisabledPreserved(const CsrReferenceInput& input, const NeutralWire& scattered, const NeutralWire& rerun)
{
  for (const ArcRecord& record : input.arcs) {
    if (!record.is_disabled && !record.is_loop_disabled) {
      continue;
    }
    const NeutralArc* before = findArc(scattered, record.key);
    const NeutralArc* after = findArc(rerun, record.key);
    require(before != nullptr && after != nullptr && *before == *after, "disabled/loop-disabled Arc changed during existing-graph rerun");
  }
}

Json coverageToJson(Json canary_receipt)
{
  requireExactKeys(canary_receipt, {"stage_fields"}, "canary receipt before provenance completion");
  canary_receipt["raw_unchanged"] = true;
  canary_receipt["fingerprints_unchanged"] = true;
  canary_receipt["disabled_arc_preserved"] = true;
  return {{"native_backed", Json::array({"chain", "diamond", "non_unate", "parallel", "disabled", "loop_disabled", "multi_root", "clock_tag", "near_tie",
                                         "post_scatter_rerun"})},
          {"synthetic_only", Json::array({"parasitic_net", "sequential_instance", "sequential_launch", "timing_check", "crpr", "propagated_clock",
                                          "clock_tree_fanout", "clock_gating_or_divider", "conditional_timing_arc", "case_analysis"})},
          {"native_fatal", Json::array({"clock_tree_overlap", "multi_driver"})},
          {"not_claimed", Json::array({"sequential_native_parity", "crpr_native_parity", "mutation", "generation_gt_zero", "cuda", "dual_csr", "speedup"})},
          {"canary_receipt", std::move(canary_receipt)}};
}

Json runReference(Database& database, std::uint64_t session_token, const Json& scenario, const ProbeOptions& options)
{
  static_cast<void>(scenario);
  require(session_token != 0, "scenario supplied a zero session token");
  Json projection_receipts = Json::array();
  RawCheckpoint pre_poison = takeRawCheckpoint(database, "pre_poison", 0, projection_receipts);
  const test::ReferencePoisonBinding binding{options.input_case_sha256, options.repeat_index, options.run_nonce, session_token};
  const std::vector<test::ReferencePoisonCanary> poison = test::poisonReferenceDerivedState(database, pre_poison.projection, binding);
  RawCheckpoint post_poison = takeRawCheckpoint(database, "post_poison", 1, projection_receipts);
  require(post_poison.bytes == pre_poison.bytes, "poison changed raw reference input");

  CsrSessionSnapshotBuildResult build = buildCsrSessionSnapshot(database, probeAnalysisContext(), session_token);
  auto* built = std::get_if<std::unique_ptr<CsrCommittedSessionSnapshot>>(&build);
  require(built != nullptr && *built != nullptr, "first snapshot build failed");
  std::unique_ptr<CsrCommittedSessionSnapshot> snapshot = std::move(*built);
  require(validateReferenceInputForSession(snapshot->input, session_token).empty(), "snapshot input is not strict-valid");
  require(snapshot->input.source_identity.session_token == session_token && snapshot->input.source_identity.graph_generation == 0,
          "snapshot identity token/generation differs");
  require(snapshot->input.unsupported_reasons.empty(), "snapshot input unexpectedly reports unsupported reasons");
  const EncodedSourceWire candidate = encodeSnapshotCandidate(snapshot->input);

  CsrComputationResult computation = computeCsrSessionReference(*snapshot);
  require(validateComputationResult(snapshot->input, computation).empty(), "reference computation result is not strict-valid");
  auto* result = std::get_if<CsrTimingResult>(&computation.outcome);
  require(result != nullptr, "supported snapshot did not produce timing result");
  const NeutralWire reference = encodeReferenceTiming(snapshot->input, *result);
  CsrTimingResultScatterAttempt scatter = scatterCsrSessionTimingResult(database, *snapshot, *result, probeAnalysisContext(), session_token);
  require(validateScatterResult(scatter.scatter_result).empty() && scatter.scatter_result.code == CsrScatterCode::kSuccess, "timing scatter did not succeed");
  const std::uint64_t expected_writes
      = 21 * static_cast<std::uint64_t>(snapshot->input.nodes.size()) + 8 * static_cast<std::uint64_t>(snapshot->input.arcs.size()) + 1;
  require(scatter.scatter_result.writes_applied == expected_writes && !scatter.scatter_result.invalidation_reason && !scatter.current_capture_error,
          "scatter success receipt differs");
  require(fingerprintsMatch(scatter.current_fingerprints, snapshot->input.source_identity), "scatter current fingerprints differ");

  const NeutralWire scattered = readNativeTiming(database, snapshot->input);
  require(scattered == reference, "immediate native readback differs from detached reference result");
  require(snapshot->active_observation && validateSnapshotObservation(*snapshot->active_observation).empty(),
          "ready post-scatter observation is absent/invalid");
  const Json ready_observation = observationToJson(*snapshot->active_observation);
  RawCheckpoint post_scatter = takeRawCheckpoint(database, "post_scatter", 2, projection_receipts);
  require(post_scatter.bytes == pre_poison.bytes, "scatter changed raw reference input");

  Json canary_receipt = installGuaranteedOverwriteCanaries(database, selectCanaries(snapshot->input, scattered));
  const NeutralWire canary = readNativeTiming(database, snapshot->input);
  require(!(canary == scattered) && !(canary == reference), "guaranteed-overwrite canary did not change the timing wire");
  RawCheckpoint post_canary = takeRawCheckpoint(database, "post_canary", 3, projection_receipts);
  require(post_canary.bytes == pre_poison.bytes, "canary changed raw reference input");

  runNativeExistingGraphTiming(database);
  verifyGuaranteedOverwriteCanaries(database, canary_receipt);
  const NeutralWire rerun = readNativeTiming(database, snapshot->input);
  require(!(rerun == canary), "native rerun retained the canary wire");
  requireDisabledPreserved(snapshot->input, scattered, rerun);
  RawCheckpoint post_rerun = takeRawCheckpoint(database, "post_rerun", 4, projection_receipts);
  require(post_rerun.bytes == pre_poison.bytes, "native rerun changed raw reference input");

  releaseCsrSessionSnapshot(*snapshot);
  require(snapshot->active_observation && validateSnapshotObservation(*snapshot->active_observation).empty(), "released observation is absent/invalid");
  const Json released_observation = observationToJson(*snapshot->active_observation);
  Json forbidden = {{"pre_poison", std::move(pre_poison.json)},     {"post_poison", std::move(post_poison.json)},
                    {"post_scatter", std::move(post_scatter.json)}, {"post_canary", std::move(post_canary.json)},
                    {"post_rerun", std::move(post_rerun.json)},     {"projection_call_receipt", std::move(projection_receipts)}};
  return {{"source_wires",
           {{"pre_poison_raw_wire_hex", bytesToHex(pre_poison.bytes)},
            {"pre_poison_raw_sha256", pre_poison.sha256},
            {"snapshot_candidate_wire_hex", candidate.wire_hex},
            {"snapshot_candidate_sha256", candidate.sha256}}},
          {"identity", identityToJson(snapshot->input.source_identity)},
          {"poison", poisonToJson(poison)},
          {"forbidden_projection", std::move(forbidden)},
          {"coverage", coverageToJson(std::move(canary_receipt))},
          {"scatter", scatterToJson(scatter)},
          {"observation", {{"ready_post_scatter", ready_observation}, {"released", released_observation}}},
          {"reference_timing", neutralWireToJson(reference)},
          {"scattered_timing", neutralWireToJson(scattered)},
          {"canary_timing", neutralWireToJson(canary)},
          {"rerun_timing", neutralWireToJson(rerun)}};
}

Json makeResult(const ProbeOptions& options, const ProbeExecution& execution)
{
  const Json& payload = execution.payload;
  return {{"schema", "ista-csr-real-differential-probe/1.0.0"},
          {"lane", "reference"},
          {"case", options.case_name},
          {"top", options.top},
          {"expected_clocks", options.expected_clocks},
          {"repeat_index", options.repeat_index},
          {"run_nonce", options.run_nonce},
          {"input_case_sha256", options.input_case_sha256},
          {"process_id", static_cast<std::uint64_t>(::getpid())},
          {"argv_receipt", makeArgvReceipt(options)},
          {"scenario", execution.scenario},
          {"source_wires", payload.at("source_wires")},
          {"identity", payload.at("identity")},
          {"poison", payload.at("poison")},
          {"forbidden_projection", payload.at("forbidden_projection")},
          {"coverage", payload.at("coverage")},
          {"scatter", payload.at("scatter")},
          {"observation", payload.at("observation")},
          {"reference_timing", payload.at("reference_timing")},
          {"scattered_timing", payload.at("scattered_timing")},
          {"canary_timing", payload.at("canary_timing")},
          {"rerun_timing", payload.at("rerun_timing")},
          {"complete", true}};
}

}  // namespace
}  // namespace ista::csr::real_diff

int main(int argc, char* argv[])
{
  using namespace ista::csr::real_diff;
  try {
    const ProbeOptions options = parseProbeOptions(argc, argv, "reference");
    const ProbeExecution execution = executeProbeScenario(
        options, [&](ista::Database& database, std::uint64_t token, const Json& scenario) { return runReference(database, token, scenario, options); });
    writeProbeResultExclusive(options, makeResult(options, execution));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ista_csr_real_reference_probe: " << error.what() << '\n';
    destroyStaInterfaceAfterFailure();
    return 1;
  }
}
