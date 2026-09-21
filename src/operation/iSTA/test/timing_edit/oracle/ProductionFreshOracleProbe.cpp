#include "IncreGpuStaBuildConfig.hpp"
// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "CsrReferenceTypes.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "CsrSnapshotObservation.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "IndependentOracleObservation.hpp"
#include "IncrementalStaSession.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeEditTransaction.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditOracleCli.hpp"
#include "TimingEditResolver.hpp"

namespace ista::csr::test {

[[nodiscard]] CsrTimingResult materializeTimingResultFromNativeOracle(
    const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle);

}  // namespace ista::csr::test

namespace ista::timing_edit::oracle {
namespace {

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("production fresh oracle probe: " + std::move(message));
}



ComponentFingerprints fingerprints(const csr::CsrSnapshotIdentity& identity)
{
  return {identity.fingerprint_schema_version, identity.topology_fingerprint,
          identity.clock_constraint_fingerprint, identity.library_fingerprint,
          identity.corner_fingerprint, identity.parasitic_fingerprint,
          identity.timing_input_fingerprint};
}

CanonicalPreStateEvidence canonicalEvidence(const Database& database,
                                            const csr::CsrReferenceInput& input)
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

BaselineSemanticIdentity baselineIdentity(const ProductionFreshSessionView& view)
{
  return {view.database.get_design_name(), fingerprints(view.reference_input.source_identity),
          canonicalEvidence(view.database, view.reference_input)};
}

std::unique_ptr<csr::CsrCommittedSessionSnapshot> buildHostSnapshot(
    const Database& database, TimingEditSessionToken token, const csr::StaAnalysisContext& context)
{
  csr::CsrSessionSnapshotBuildResult result
      = csr::buildCsrSessionSnapshot(database, context, token.value());
  auto* snapshot
      = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&result);
  if (snapshot == nullptr || *snapshot == nullptr) {
    fail("host full-CSR snapshot build failed");
  }
  return std::move(*snapshot);
}

struct CsrFullEqualityReceipt
{
  std::size_t ordinal{0};
  std::size_t node_count{0};
  std::size_t arc_count{0};
};

CsrFullEqualityReceipt requireCsrFullExact(
    csr::CsrCommittedSessionSnapshot& snapshot, const Database& database,
    std::size_t ordinal)
{
  StaTimingEditSessionProbeAccess::requireCsrFullExact(snapshot, database);
  return {ordinal, snapshot.input.nodes.size(), snapshot.input.arcs.size()};
}

csr::NodeKey baselineNode(const TimingEditTrace& trace, std::string_view full_name)
{
  const auto found = std::ranges::find_if(
      trace.baseline.canonical_pre_state.nodes,
      [&](const csr::NodeKey& key) { return key.full_name == full_name; });
  if (found == trace.baseline.canonical_pre_state.nodes.end()) {
    fail("wrong-sink subject node is absent from the baseline trace");
  }
  return *found;
}

DesignEditPlan planFor(const TimingEditTrace& trace, std::size_t index,
                       const CommittedEditIdentity& current,
                       OracleSubject subject)
{
  const TimingEditTraceBatch& batch = trace.batches.at(index);
  DesignEditPlan plan{.baseline = trace.baseline,
                      .expected_generation = current.generation,
                      .edits = batch.edits};
  if (subject == OracleSubject::kWrongSink) {
    const StableNodeRef wrong_sink = ExistingNodeRef{baselineNode(trace, "u2:A")};
    for (BufferInsertionMutation& edit : plan.edits) {
      edit.moved_sinks = {wrong_sink};
    }
    if (index == 0) {
      plan.edits.front().original_net = ExistingNetRef{"n1"};
    }
  }
  return plan;
}

std::vector<ElectricalLoadObservation> captureElectricalLoads(Database& database)
{
  std::vector<ElectricalLoadObservation> result;
  constexpr AnalysisType analyses[]{AnalysisType::kMax, AnalysisType::kMin};
  constexpr TransType transitions[]{TransType::kRise, TransType::kFall};
  STADC.init();
  try {
    for (auto& [name, net] : database.get_net_map()) {
      static_cast<void>(name);
      if (net.get_driver_pin().empty()) {
        continue;
      }
      for (AnalysisType analysis : analyses) {
        for (TransType transition : transitions) {
          std::string output_pin = net.get_driver_pin();
          const double load = STADC.getPowerOutputLoad(output_pin, analysis, transition);
          if (!std::isfinite(load) || load < 0.0) {
            fail("native electrical output load is invalid");
          }
          result.push_back({output_pin, static_cast<std::int32_t>(analysis),
                            static_cast<std::int32_t>(transition), load});
        }
      }
    }
    STADC.destroy();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    try {
      STADC.destroy();
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
  return result;
}

IndependentReplayReceipt observationReceipt(const TimingEditTrace& trace,
                                             std::size_t prefix,
                                             const TimingEditResolverState& resolver,
                                             Database& database)
{
  IndependentReplayReceipt receipt;
  receipt.prefix_batches = prefix;
  for (std::size_t index = 0; index < prefix; ++index) {
    for (const BufferInsertionMutation& edit : trace.batches[index].edits) {
      const TokenMaterialization& materialization
          = resolver.materialization(edit.creation_token);
      receipt.materialized_tokens.push_back(
          {edit.creation_token, materialization.instance_name,
           materialization.output_net_name, materialization.input_pin_name,
           materialization.output_pin_name});
    }
  }
  receipt.electrical_loads = captureElectricalLoads(database);
  return receipt;
}

std::string csrReceiptText(const std::vector<CsrFullEqualityReceipt>& receipts)
{
  std::string text = "schema|ista-timing-edit-csr-full-receipt/1.0.0\n";
  for (const CsrFullEqualityReceipt& receipt : receipts) {
    text += "csr_full_exact|" + std::to_string(receipt.ordinal) + "|"
            + std::to_string(receipt.node_count) + "|"
            + std::to_string(receipt.arc_count) + "|entire_csr_timing_result\n";
  }
  return text;
}

void installDerivedTimingTamper(Database& database)
{
  auto& points = database.get_timing_point_map();
  if (points.empty()) {
    fail("derived-tamper subject found no native timing point");
  }
  auto& point = points.begin()->second;
  const double before = point.get_arrival();
  point.set_arrival(before == 0.0 ? 1.0 : 0.0);
}

void requireExactGenerationZeroAfterTamper(
    const ProductionFreshSessionView& view, const TimingEditResolverState& resolver,
    const CommittedEditIdentity& identity, const CommittedEditIdentity& initial_identity,
    const TimingEditResolverState& initial_resolver,
    const NativeEditTransaction& installed, const RawDesignObservation& raw_baseline)
{
  const auto active_identity = NativeTimingEditPreparer::captureCurrentIdentity(
      STADM.getDatabase(), TransactionGeneration{0}, view.reference_input.analysis_context,
      TimingEditSessionToken{view.session_token});
  const auto raw_mismatches = compareRawDesign(raw_baseline, captureRawDesign(STADM.getDatabase()));
  if (installed.active() || identity != initial_identity || resolver != initial_resolver
      || active_identity != initial_identity || !raw_mismatches.empty()
      || view.session.state() != StaSessionState::kBaselineExecuted
      || !view.authority.active())
    fail("derived-tamper rollback did not preserve exact generation zero");
}

std::string derivedTamperReceiptText()
{
  return "schema|ista-timing-edit-derived-tamper-receipt/2.0.0\n"
         "derived_tamper_detected|csr_full_exact|prepublish|"
         "published=false|database_generation=0|raw=unchanged|"
         "transaction_generation=0\n";
}

void applyPrefix(const ProductionFreshSessionView& view, const TimingEditTrace& trace,
                 const OracleReplayOptions& options,
                 const RawDesignObservation& raw_baseline)
{
  const TimingEditSessionToken token{view.session_token};
  const BaselineSemanticIdentity actual_baseline = baselineIdentity(view);
  if (actual_baseline != trace.baseline) {
    fail("trace baseline identity differs from the freshly loaded Database/CSR state");
  }
  const CommittedEditIdentity initial_identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          view.database, TransactionGeneration{0}, view.reference_input.analysis_context, token);
  if (initial_identity
      != CommittedEditIdentity{TransactionGeneration{0}, trace.baseline.fingerprints,
                               trace.baseline.canonical_pre_state}) {
    fail("production generation-zero identity differs from the durable trace");
  }

  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(view.database);
  const TimingEditResolverState initial_resolver = resolver;
  auto identity = initial_identity;
  {
    std::unique_ptr<csr::CsrSnapshotObservation> released;
    StaTimingEditSessionProbeAccess::releaseSnapshotForMutation(view.session_context, &released);
  }
  std::vector<CsrFullEqualityReceipt> csr_receipts;
  csr_receipts.reserve(options.prefix);
  const TimingEditPrepareContext prepare_context{
      token, trace.baseline, identity, view.reference_input.analysis_context};
  for (std::size_t index = 0; index < options.prefix; ++index) {
    auto plan = planFor(trace, index, identity, options.subject);
    auto prepared = NativeTimingEditPreparer::prepare(
        STADM.getDatabase(), plan, prepare_context, resolver);
    PreparedTimingEditResolverPatch resolver_patch(resolver, resolver.prepare(prepared));
    auto installed = NativeEditTransaction::prepareFull(
        view.authority, prepared, resolver_patch.view());
    NativeCandidateExecutor::executeFull(installed);
    resolver_patch.apply();
    auto host_snapshot = buildHostSnapshot(installed.database(), token, view.reference_input.analysis_context);
    if (options.subject == OracleSubject::kDerivedTamper) {
      installDerivedTimingTamper(installed.database());
      bool mismatch = false;
      try {
        static_cast<void>(requireCsrFullExact(*host_snapshot, installed.database(), index + 1));
      } catch (const CsrFullExactMismatch&) {
        mismatch = true;
      }
      if (!mismatch)
        fail("derived timing tamper escaped the independent native/CSR comparison");
      if (!installed.rollback())
        fail("derived-tamper rollback failed");
      resolver_patch.rollback();
      requireExactGenerationZeroAfterTamper(
          view, resolver, identity, initial_identity, initial_resolver, installed, raw_baseline);
      writeOracleFile(options.output, derivedTamperReceiptText());
      return;
    }
    csr_receipts.push_back(requireCsrFullExact(*host_snapshot, installed.database(), index + 1));
    auto post_identity = NativeTimingEditPreparer::captureCurrentIdentity(
        installed.database(), prepared.targetGeneration(),
        view.reference_input.analysis_context, token);
    if (options.subject == OracleSubject::kCorrect
        && post_identity != trace.batches[index].expected_post_identity)
      fail("production post identity differs from the durable trace");
    installed.commit();
    resolver_patch.commit();
    std::swap(identity, post_identity);
  }

  Database& committed_database = STADM.getDatabase();
  const RawDesignObservation observed = captureRawDesign(committed_database);
  if (options.subject == OracleSubject::kCorrect) {
    const RawDesignObservation expected
        = projectExpectedRawAfterPrefix(raw_baseline, trace, options.prefix);
    const std::vector<std::string> raw_mismatches = compareRawDesign(expected, observed);
    if (!raw_mismatches.empty()) {
      fail("production raw replay differs at " + raw_mismatches.front());
    }
  }

  const IndependentReplayReceipt receipt = observationReceipt(
      trace, options.prefix, resolver, committed_database);
  const std::vector<std::string> derived_errors
      = validateDerivedDatabase(committed_database, receipt);
  if (!derived_errors.empty()) {
    fail("production derived timing replay differs: " + derived_errors.front());
  }
  writeOracleFile(
      options.output,
      canonicalOracleObservationText(captureOracleObservation(committed_database, receipt)));
  writeOracleFile(options.output.string() + ".csr-full.txt",
                  csrReceiptText(csr_receipts));
}

void runImplemented(const OracleReplayOptions& options, const TimingEditTrace& trace)
{
  ProductionFreshSessionLoader::run(
      options.artifacts, [&](const ProductionFreshSessionView& view) {
        requireTraceSourceArtifacts(trace, view.artifact_evidence);
        const RawDesignObservation raw_baseline = captureRawDesign(view.database);
        applyPrefix(view, trace, options, raw_baseline);
      });
}

void run(const OracleReplayOptions& options)
{
  const TimingEditTrace trace = readStrictTimingEditTrace(options.trace);
  if (options.prefix > trace.batches.size()) {
    fail("requested prefix is outside the durable trace");
  }
  if (options.subject == OracleSubject::kDerivedTamper
      && options.prefix != 1) {
    fail("derived-tamper subject requires exactly one batch");
  }
  const OracleModeStatus mode_status = statusFor(options.mode);
  if (mode_status.disposition == OracleModeDisposition::kUnsupported) {
    if (options.subject != OracleSubject::kCorrect) {
      fail("unsupported modes do not accept subject fault injection");
    }
    writeOracleFile(options.output, canonicalOracleModeStatusText(mode_status));
    return;
  }
  runImplemented(options, trace);
}

}  // namespace
}  // namespace ista::timing_edit::oracle

int main(int argc, char* argv[])
{
  using namespace ista::timing_edit::oracle;
  try {
    if constexpr (!ista::incregpusta_debug_checks_enabled)
      throw std::runtime_error("diagnostic probe requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
    OracleCliMode mode = parseOracleCli(argc, argv, false);
    run(std::get<OracleReplayOptions>(mode));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
