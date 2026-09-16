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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceTypes.hpp"
#include "Database.hpp"
#include "IndependentFreshBaselineLoader.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "IndependentOracleObservation.hpp"
#include "IndependentSemanticReplayer.hpp"
#include "../../PublicTimingJson.hpp"
#include "TimingSemanticKey.hpp"
#include "TimingEditOracleCli.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::timing_edit::oracle {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("independent fresh oracle probe: " + std::move(message));
}

ComponentFingerprints fingerprints(const csr::CsrSnapshotIdentity& identity)
{
  return {identity.fingerprint_schema_version, identity.topology_fingerprint, identity.clock_constraint_fingerprint,
          identity.library_fingerprint, identity.corner_fingerprint, identity.parasitic_fingerprint,
          identity.timing_input_fingerprint};
}

CanonicalPreStateEvidence baselineEvidence(const Database& database, const csr::CsrReferenceInput& input)
{
  CanonicalPreStateEvidence result;
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(instance);
    result.instances.push_back(name);
  }
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    result.nets.push_back(name);
  }
  for (const csr::NodeRecord& node : input.nodes) {
    result.nodes.push_back(node.key);
  }
  for (const csr::ArcRecord& arc : input.arcs) {
    result.arcs.push_back(arc.key);
  }
  std::ranges::sort(result.nodes, csr::nodeKeyLess);
  std::ranges::sort(result.arcs, csr::arcKeyLess);
  return result;
}

CommittedEditIdentity currentIdentity(const FreshBaselineView& view,
                                      TransactionGeneration generation)
{
  csr::CsrReferenceInputExportResult exported = csr::exportCsrReferenceInputCandidate(
      view.database, view.reference_input.analysis_context, view.session_token);
  const auto* input = std::get_if<csr::CsrReferenceInput>(&exported);
  if (input == nullptr || !input->unsupported_reasons.empty()
      || input->source_identity.session_token != view.session_token
      || input->source_identity.graph_generation != 0) {
    fail("current Database cannot be exported as a supported full CSR input");
  }
  return {generation, fingerprints(input->source_identity),
          baselineEvidence(view.database, *input)};
}

BaselineSemanticIdentity baselineIdentity(const FreshBaselineView& view)
{
  return {view.database.get_design_name(), fingerprints(view.reference_input.source_identity),
          baselineEvidence(view.database, view.reference_input)};
}

void requireBaselineIdentity(const FreshBaselineView& view, const TimingEditTrace& trace)
{
  if (baselineIdentity(view) != trace.baseline) {
    fail("trace baseline identity differs from the freshly loaded Database/CSR state");
  }
}

csr::NodeKey requireNode(const csr::CsrReferenceInput& input, std::string_view full_name)
{
  const auto found = std::ranges::find_if(input.nodes, [&](const csr::NodeRecord& node) {
    return node.key.full_name == full_name;
  });
  if (found == input.nodes.end()) {
    fail("baseline semantic node is absent: " + std::string(full_name));
  }
  return found->key;
}

LibraryCellRef requireBuffer(const FreshBaselineView& view, const OracleTraceCreationOptions& options)
{
  const TimingCell& cell = view.database.get_timing_library().get_cell_map().at(options.buffer);
  LibraryCellRef reference{cell.get_library_name(), options.buffer, options.input_port, options.output_port, {}};
  for (const auto& arc : cell.get_cell_arc_list())
    if (arc.get_is_timing_graph_arc() && !arc.get_is_disable_arc() && !arc.get_is_clock_arc()
        && !arc.get_is_clear_preset_arc() && arc.get_source_port() == options.input_port && arc.get_sink_port() == options.output_port)
      for (const auto& child : arc.get_timing_arc_list())
        reference.ordered_arc_signatures.push_back(csr::detail::exportReferenceLibraryArcSignature(child));
  // IndependentSemanticReplayer validates the selected cell before its first edit.
  return reference;
}

TimingEditTrace makeRealTrace(const FreshBaselineView& view,
                              const OracleTraceCreationOptions& options)
{
  const auto& profile = options.profile;
  const auto seed = options.seed;
  const BaselineSemanticIdentity baseline = baselineIdentity(view);
  const CreationToken first_token{1, view.database.get_design_name(), profile,
                                  seed, 1, 0};
  const CreationToken second_token{1, view.database.get_design_name(), profile,
                                   seed, 2, 0};
  const LibraryCellRef library = requireBuffer(view, options);
  const StableNodeRef sink = ExistingNodeRef{requireNode(view.reference_input, options.sink)};
  std::vector<BufferInsertionMutation> edits;
  edits.push_back({ExistingNetRef{options.net}, {sink}, library, UnplacedInputs{},
                   NoParasiticRcInputs{}, first_token});
  edits.push_back({CreatedNetRef{first_token, CreatedNetRole::kOutputNet}, {sink}, library,
                   UnplacedInputs{}, NoParasiticRcInputs{}, second_token});

  TimingEditTrace trace;
  trace.workload_kind = TraceWorkloadKind::kRealDesign;
  trace.seed = seed;
  trace.profile = profile;
  trace.baseline = baseline;
  for (const FreshArtifactEvidence& artifact : view.artifact_evidence) {
    trace.source_artifacts.push_back({artifact.role, artifact.logical_name, artifact.content_sha256});
  }

  CommittedEditIdentity before{TransactionGeneration{0}, baseline.fingerprints,
                               baseline.canonical_pre_state};
  for (std::size_t index = 0; index < edits.size(); ++index) {
    const std::uint64_t ordinal = index + 1;
    DesignEditPlan plan{.baseline = baseline, .expected_generation = before.generation,
                        .edits = {edits[index]}};
    static_cast<void>(IndependentSemanticReplayer::replayPlan(view.database, plan));
    const CommittedEditIdentity after
        = currentIdentity(view, TransactionGeneration{ordinal});
    trace.batches.push_back(
        {ordinal, before, {std::move(edits[index])}, PolicyReceipt{}, after});
    before = after;
  }

  const fs::path replay_root = options.trace_output.parent_path();
  trace.replay_command = {
      options.executable.string(),
      makeReplayArguments(options.artifacts, options.trace_output, trace.batches.size(),
                          replay_root / "recorded-replay-observation.txt",
                          replay_root / "recorded-replay-run")};
  const TimingEditContractViolations errors = validateTimingEditTrace(trace);
  if (!errors.empty()) {
    fail("constructed real trace is invalid: " + errors.front().field + ": " + errors.front().message);
  }
  return trace;
}

void createTrace(const OracleTraceCreationOptions& options)
{
  IndependentFreshBaselineLoader::run(options.artifacts, [&](const FreshBaselineView& view) {
    const TimingEditTrace trace = makeRealTrace(view, options);
    writeOracleFile(options.trace_output, timingEditTraceToJson(trace) + "\n");
  });
}

void runReal(const OracleReplayOptions& options)
{
  if (options.mode != OracleExecutionMode::kCpuFull
      || options.subject != OracleSubject::kCorrect) {
    fail("independent oracle supports only the correct cpu/full lane");
  }
  const TimingEditTrace trace = readStrictTimingEditTrace(options.trace);
  if (options.prefix > trace.batches.size()) {
    fail("requested prefix is outside the durable trace");
  }
  IndependentFreshBaselineLoader::run(options.artifacts, [&](const FreshBaselineView& view) {
    requireTraceSourceArtifacts(trace, view.artifact_evidence);
    requireBaselineIdentity(view, trace);
    const RawDesignObservation baseline = captureRawDesign(view.database);
    const IndependentReplayReceipt receipt
        = IndependentSemanticReplayer::replayPrefix(view.database, trace, options.prefix);
    const CommittedEditIdentity actual_post
        = currentIdentity(view, TransactionGeneration{options.prefix});
    if (actual_post != trace.batches[options.prefix - 1].expected_post_identity) {
      fail("replayed post identity differs from the durable trace");
    }
    const RawDesignObservation expected = projectExpectedRawAfterPrefix(baseline, trace, options.prefix);
    const RawDesignObservation observed = captureRawDesign(view.database);
    const std::vector<std::string> raw_mismatches = compareRawDesign(expected, observed);
    if (!raw_mismatches.empty()) {
      fail("raw replay differs at " + raw_mismatches.front());
    }
    const std::vector<std::string> derived_errors = validateDerivedDatabase(view.database, receipt);
    if (!derived_errors.empty()) {
      fail("derived timing replay differs: " + derived_errors.front());
    }
    writeOracleFile(options.output,
                    canonicalOracleObservationText(captureOracleObservation(view.database, receipt)));
    writeOracleFile(options.output.string() + ".public.json", ista::test::capturePublicTiming().dump(2));
  });
}

TimingEditTrace makeWrongSinkSelfTestTrace()
{
  TimingEditTrace trace;
  trace.batches.resize(1);
  CreationToken creation{1, "wrong_sink_selftest", "stage3_real_cpu_full", 7, 1, 0};
  csr::LibraryArcSignature signature;
  signature.library_name = "selftest_lib";
  signature.arc_index = 0;
  trace.batches[0].edits.push_back(
      {ExistingNetRef{"n0"},
       {ExistingNodeRef{{csr::NodeKind::kPort, "sink_expected", {}, {}}}},
       {"selftest_lib", "BUF_X1", "A", "Y", {signature}}, UnplacedInputs{}, NoParasiticRcInputs{}, creation});
  return trace;
}

int wrongSinkSelfTest()
{
  RawDesignObservation baseline;
  baseline.design_name = "wrong_sink_selftest";
  baseline.pins = {{"sink_expected", "sink_expected", "sink_expected", {}, "n0",
                    static_cast<std::int32_t>(PinDirection::kOutput), 0.0, 0.0, true},
                   {"sink_wrong", "sink_wrong", "sink_wrong", {}, "n0",
                    static_cast<std::int32_t>(PinDirection::kOutput), 0.0, 0.0, true},
                   {"src", "src", "src", {}, "n0", static_cast<std::int32_t>(PinDirection::kInput), 0.0, 0.0, true}};
  baseline.nets = {{"n0", "n0", "src", {"src"}, {"sink_expected", "sink_wrong"},
                    {"src", "sink_expected", "sink_wrong"}}};
  const TimingEditTrace trace = makeWrongSinkSelfTestTrace();
  const RawDesignObservation expected = projectExpectedRawAfterPrefix(baseline, trace, 1);
  const auto source_net = std::ranges::find(expected.nets, "n0", &RawNetObservation::map_name);
  if (source_net == expected.nets.end()
      || source_net->load_pins != std::vector<std::string>{"__ista_buf_b1_e0:A", "sink_wrong"})
    fail("expected net caches do not preserve native sorted load order");
  RawDesignObservation wrong = expected;
  const std::string instance = "__ista_buf_b1_e0";
  const std::string new_net = "__ista_net_b1_e0";
  for (RawPinObservation& pin : wrong.pins) {
    if (pin.map_name == "sink_expected") {
      pin.net_name = "n0";
    } else if (pin.map_name == "sink_wrong") {
      pin.net_name = new_net;
    }
  }
  for (RawNetObservation& net : wrong.nets) {
    if (net.map_name == "n0") {
      net.pin_names = {"src", "sink_expected", instance + ":A"};
      net.driver_pin = "src";
      net.driver_pins = {"src"};
      net.load_pins = {"sink_expected", instance + ":A"};
    } else if (net.map_name == new_net) {
      net.pin_names = {instance + ":Y", "sink_wrong"};
      net.driver_pin = instance + ":Y";
      net.driver_pins = {instance + ":Y"};
      net.load_pins = {"sink_wrong"};
    }
  }
  const std::vector<std::string> mismatches = compareRawDesign(expected, wrong);
  const bool caught_expected_sink = std::ranges::find(mismatches, "raw.pin.sink_expected") != mismatches.end();
  const bool caught_wrong_sink = std::ranges::find(mismatches, "raw.pin.sink_wrong") != mismatches.end();
  if (mismatches.empty() || !caught_expected_sink || !caught_wrong_sink) {
    std::cerr << "wrong-sink subject escaped the independent raw oracle\n";
    return 1;
  }
  std::cout << "independent oracle detected deliberate subject wrong-sink replay\n";
  return 0;
}

}  // namespace
}  // namespace ista::timing_edit::oracle

int main(int argc, char* argv[])
{
  using namespace ista::timing_edit::oracle;
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test-wrong-sink") {
      return wrongSinkSelfTest();
    }
    OracleCliMode mode = parseOracleCli(argc, argv, true);
    if (const auto* create = std::get_if<OracleTraceCreationOptions>(&mode)) {
      createTrace(*create);
    } else {
      runReal(std::get<OracleReplayOptions>(mode));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
