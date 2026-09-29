#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "NarrowTimingRecordReader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "Database.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrStructuredBatchPreparer.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "NativeDualCsrPayloadCaptureInternal.hpp"
#include "TimingEditDualCsrCaptureEvidenceInternal.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPoint.hpp"
#include "TimingPortConstraint.hpp"

namespace {

using namespace ista;
using namespace ista::timing_dual_csr;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

constexpr TimingEditSessionToken kSession{71};
constexpr TransactionGeneration kGeneration{0};

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error("narrow capture test: " + message);
}

template <typename T>
T requireAlternative(auto value, const std::string& message)
{
  auto* result = std::get_if<T>(&value);
  require(result != nullptr, message);
  return std::move(*result);
}

bool allEqual(const csr::ArcOutputLoadMatrix& values, double expected)
{
  return std::ranges::all_of(values, [&](const auto& analysis) {
    return std::ranges::all_of(
        analysis, [&](double value) { return value == expected; });
  });
}

std::size_t addArc(Database& database, const std::string& source,
                   const std::string& sink, ArcType kind,
                   const std::string& owner)
{
  Arc arc;
  arc.set_arc_name(owner + ":" + source + "->" + sink);
  arc.set_source_pin(source);
  arc.set_sink_pin(sink);
  arc.set_owner_name(owner);
  arc.set_type(kind);
  database.get_arc_list().push_back(std::move(arc));
  const std::size_t index = database.get_arc_list().size() - 1;
  database.get_outgoing_arc_list_map()[source].push_back(index);
  database.get_incoming_arc_list_map()[sink].push_back(index);
  return index;
}

void addNetArc(Database& database, const std::string& source,
               const std::string& sink, const std::string& net)
{
  static_cast<void>(addArc(database, source, sink, ArcType::kNet, net));
}

void addCellArc(Database& database, const std::string& instance,
                const std::string& cell_name)
{
  TimingCellArc& cell_arc = database.get_timing_library()
                                .get_cell_map()
                                .at(cell_name)
                                .get_cell_arc_list()
                                .front();
  const std::string source = instance + ":" + cell_arc.get_source_port();
  const std::string sink = instance + ":" + cell_arc.get_sink_port();
  Arc& arc = database.get_arc_list().at(
      addArc(database, source, sink, ArcType::kCell, instance));
  arc.set_library_source_port(cell_arc.get_source_port());
  arc.set_library_sink_port(cell_arc.get_sink_port());
  arc.set_timing_cell_arc(&cell_arc);
}

struct Baseline
{
  NativeEditFixture fixture;
  TimingEditResolverState resolver;
  DualCsrState state;
  DualCsrArcGroupIndex arc_group_index;
  CommittedEditIdentity identity;
  BaselineSemanticIdentity baseline;
};

Baseline makeBaseline()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  fixture.database()
      .get_timing_library()
      .get_cell_map()
      .at("BUF_X1")
      .get_port_map()
      .at("A")
      .set_capacitance(0.17);
  TimingPortConstraint output;
  output.set_port_name("out");
  output.set_load(0.31);
  output.set_has_load(true);
  fixture.database()
      .get_timing_constraint()
      .get_port_constraint_map()
      .emplace("out", std::move(output));

  TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(fixture.database());
  auto exported = csr::exportCsrReferenceInputCandidate(
      fixture.database(), editTestAnalysisContext(), kSession.value());
  csr::CsrReferenceInput input
      = requireAlternative<csr::CsrReferenceInput>(
          std::move(exported), "baseline reference export failed");
  auto baseline_attempt = DualCsrBaselineFactory::create(
      input, kSession, resolver.highWater());
  require(baseline_attempt.success(),
          "Dual-CSR baseline creation failed");
  DualCsrState state = std::move(*baseline_attempt.state);

  auto bindings = captureFullDualCsrArcGroupBindings(resolver);
  require(bindings.success(), "baseline resolver binding capture failed");
  auto index_attempt = buildDualCsrArcGroupIndex(state, bindings.bindings);
  require(index_attempt.success(), "baseline arc-group index failed");

  CommittedEditIdentity identity
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), kGeneration, editTestAnalysisContext(), kSession);
  BaselineSemanticIdentity baseline{
      fixture.database().get_design_name(), identity.current_fingerprints,
      identity.canonical_pre_state};
  return {std::move(fixture), std::move(resolver), std::move(state),
          std::move(*index_attempt.index), std::move(identity),
          std::move(baseline)};
}

PendingStructuredBatch prepareBuffer(const Baseline& baseline,
                                     DesignEditPlan plan)
{
  const DualSemanticReadView view{baseline.state};
  return DualCsrStructuredBatchPreparer::prepare(
      std::move(plan), baseline.resolver, view, baseline.state,
      baseline.identity, baseline.arc_group_index);
}

PendingStructuredBatch prepareBuffer(const Baseline& baseline)
{
  return prepareBuffer(
      baseline,
      bufferInsertionPlan(baseline.fixture.database(), baseline.baseline,
                          kGeneration, editTestCreationToken()));
}

void materializePostGraph(Database& database,
                          const TokenMaterialization& names)
{
  database.get_timing_point_map().clear();
  database.get_arc_list().clear();
  database.get_outgoing_arc_list_map().clear();
  database.get_incoming_arc_list_map().clear();
  const std::vector<std::string> order{
      "in", names.input_pin_name, names.output_pin_name, "out"};
  for (std::size_t index = 0; index < order.size(); ++index) {
    database.get_timing_point_map().emplace(order[index], TimingPoint{});
    database.get_timing_point_map().at(order[index]).set_level(
        static_cast<std::int32_t>(index + 1));
  }
  database.get_timing_order_list() = order;
  database.get_start_point_list() = {"in"};
  database.get_end_point_list() = {"out"};

  auto& original = database.get_net_map().at("n0");
  original.set_driver_pin("in");
  original.set_driver_pin_list({"in"});
  original.set_load_pin_list({names.input_pin_name});
  auto& output = database.get_net_map().at(names.output_net_name);
  output.set_driver_pin(names.output_pin_name);
  output.set_driver_pin_list({names.output_pin_name});
  output.set_load_pin_list({"out"});

  addNetArc(database, "in", names.input_pin_name, "n0");
  addCellArc(database, names.instance_name, "BUF_X1");
  addNetArc(database, names.output_pin_name, "out",
            names.output_net_name);
}

void addUnindexedMalformedCanary(Database& database)
{
  Arc canary;
  canary.set_arc_name("unaffected-malformed-canary");
  canary.set_source_pin("missing-source");
  canary.set_sink_pin("missing-sink");
  canary.set_owner_name("missing-owner");
  canary.set_type(ArcType::kNet);
  database.get_arc_list().push_back(std::move(canary));
}

CandidateBoundCapture read(
    const Baseline& baseline, const Database& candidate,
    const PendingStructuredBatch& pending,
    const AffectedCapturePlan& plan)
{
  csr::detail::NativeLibraryModels models(candidate);
  return NarrowTimingRecordReader{models}.read(
      candidate, DualSemanticReadView{baseline.state},
      baseline.arc_group_index, pending.stagedResolver(),
      pending.preparedBatch(), plan);
}

void requireExactAudit(const AffectedCapturePlan& plan,
                       const NarrowCaptureAudit& audit,
                       std::size_t native_arc_count)
{
  const auto active = static_cast<std::uint64_t>(std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity != 0;
      }));
  const auto deactivated = static_cast<std::uint64_t>(
      plan.arcs.size()) - active;
  require(audit.requested_nodes == plan.nodes.size()
              && audit.requested_instances
                     == plan.token_materializations.size()
              && audit.requested_arcs == active
              && audit.requested_deactivations == deactivated
              && audit.requested_load_nets == plan.load_nets.size()
              && audit.requested_node_roles == plan.node_roles.size()
              && audit.requested_cell_pin_capacitances
                     == plan.cell_pin_capacitances.size(),
          "audit requests do not equal the frozen typed plan");
  require(audit.accepted_nodes == audit.requested_nodes
              && audit.accepted_instances == audit.requested_instances
              && audit.accepted_arcs == audit.requested_arcs
              && audit.accepted_deactivations
                     == audit.requested_deactivations
              && audit.accepted_load_nets == audit.requested_load_nets
              && audit.accepted_library_cells
                     == audit.requested_library_cells
              && audit.accepted_node_roles == audit.requested_node_roles
              && audit.accepted_cell_pin_capacitances
                     == audit.requested_cell_pin_capacitances,
          "audit accepted a non-bijective record set");
  require(audit.captured_arc_records == plan.arcs.size()
              && audit.deactivated_arc_before_reads == deactivated
              && audit.native_arc_candidate_reads
                     < static_cast<std::uint64_t>(native_arc_count)
              && audit.rejected_reads == 0
              && audit.full_export_guard_active
              && !audit.full_export_observed
              && audit.native_full_graph_exports == 0
              && audit.native_full_reference_exports == 0,
          "audit is not an exact guarded local-read proof");
}

bool descriptorHasLoad(const DualCsrBatchDescriptor& descriptor,
                       double expected)
{
  const auto matches = [&](const csr::ArcSemanticPayload& payload) {
    return payload.output_load
           && allEqual(*payload.output_load, expected);
  };
  return std::ranges::any_of(
             descriptor.arc_appends,
             [&](const auto& arc) { return matches(arc.payload); })
         || std::ranges::any_of(
             descriptor.payload_changes,
             [&](const auto& change) {
               return matches(change.replacement);
             });
}

void checkCapacitanceEvidenceLookup(const Baseline& baseline,
                                    const PendingStructuredBatch& pending)
{
  csr::detail::NativeLibraryModels models(baseline.fixture.database());
  auto capture = ista::timing_dual_csr::detail::captureNativeDualCsrPayload(
      baseline.fixture.database(), models, DualSemanticReadView{baseline.state},
      baseline.arc_group_index, pending.stagedResolver(), pending.capturePlan());
  require(capture.success(), "model-evidence fixture capture failed");
  auto records = std::move(*capture.records);
  const auto validate = [&] {
    return ista::timing_dual_csr::detail::validateStructuredNodeModelEvidence(pending, records);
  };
  require(!validate(), "valid capacitance evidence was rejected");
  std::ranges::reverse(records.nodes);
  std::ranges::reverse(records.cell_pin_capacitances);
  require(!validate(), "capacitance lookup depends on capture storage order");
  require(records.cell_pin_capacitances.size() >= 2, "capacitance fixture needs two identities");
  const auto saved = records.cell_pin_capacitances;
  records.cell_pin_capacitances.back() = records.cell_pin_capacitances.front();
  require(validate() == std::optional<std::string>{"pin-capacitance evidence repeats one typed request"},
          "duplicate capacitance identity was accepted");
  records.cell_pin_capacitances = saved;
  records.cell_pin_capacitances.front().requested.expected_port += "_foreign";
  require(validate() == std::optional<std::string>{"pin-capacitance evidence has a mismatched typed identity"},
          "lookup matched only the ref and ignored the complete request");
  records.cell_pin_capacitances = saved;
  records.cell_pin_capacitances.front().model.default_capacitance += 1.0;
  require(validate() == std::optional<std::string>{"pin-capacitance evidence differs from its library port"},
          "capacitance lookup bypassed the library-value check");
}

void checkStableReresolutionAndBoundaryCanaries()
{
  Baseline baseline = makeBaseline();
  PendingStructuredBatch pending = prepareBuffer(baseline);
  require(NativeDesignEditTranslator::applyNormalizedBatch(
              baseline.fixture.database(), pending.preparedBatch(),
              pending.stagedResolver())
              .applied(),
          "candidate translation failed");
  materializePostGraph(
      baseline.fixture.database(),
      pending.preparedBatch().tokenMaterializations().front());
  addUnindexedMalformedCanary(baseline.fixture.database());

  const AffectedCapturePlan& plan = pending.capturePlan();
  auto duplicate = plan;
  duplicate.arcs.push_back(duplicate.arcs.front());
  auto rejected = read(baseline, baseline.fixture.database(), pending, duplicate);
  require(rejected.error() == NarrowCaptureError::kDuplicateRequest
              && rejected.audit() != nullptr
              && !rejected.audit()->full_export_guard_active,
          "duplicate stable request was not rejected before capture");

  auto malformed_kind = plan;
  require(malformed_kind.node_roles.size() >= 2, "fixture needs two model requests");
  malformed_kind.node_roles[1].expected_key.kind = static_cast<csr::NodeKind>(255);
  rejected = read(baseline, baseline.fixture.database(), pending, malformed_kind);
  require(rejected.error() == NarrowCaptureError::kUnplannedDependency,
          "malformed second model key escaped the typed error boundary");

  auto wrong_multiplicity = plan;
  auto active = std::ranges::find_if(
      wrong_multiplicity.arcs,
      [](const auto& arc) { return arc.expected_post_multiplicity != 0; });
  require(active != wrong_multiplicity.arcs.end(),
          "fixture has no active arc request");
  ++active->expected_post_multiplicity;
  rejected = read(baseline, baseline.fixture.database(), pending,
                  wrong_multiplicity);
  require(rejected.error()
              == NarrowCaptureError::kArcMultiplicityMismatch,
          "wrong post multiplicity was accepted");

  auto foreign_ref = plan;
  foreign_ref.arcs.front().ref = ExistingArcRef{csr::ArcKey{
      foreign_ref.arcs.front().expected_post_base,
      std::numeric_limits<std::uint64_t>::max()}};
  rejected = read(baseline, baseline.fixture.database(), pending, foreign_ref);
  require(rejected.error() == NarrowCaptureError::kBatchBindingMismatch,
          "foreign stable arc ref was accepted");

  auto rebound = plan;
  rebound.target_generation = TransactionGeneration{9};
  rejected = read(baseline, baseline.fixture.database(), pending, rebound);
  require(rejected.error() == NarrowCaptureError::kBatchBindingMismatch,
          "typed exact plan/batch mismatch was accepted");

  checkCapacitanceEvidenceLookup(baseline, pending);
  CandidateBoundCapture capture
      = read(baseline, baseline.fixture.database(), pending, plan);
  require(capture.success() && capture.audit() != nullptr,
          "valid narrow capture was rejected");
  const NarrowCaptureAudit audit = *capture.audit();
  requireExactAudit(plan, audit,
                    baseline.fixture.database().get_arc_list().size());

  VerifiedStructuredBatch verified
      = TimingEditDualCsrDescriptorBuilder::build(
          std::move(pending), std::move(capture));
  require(verified.captureAudit() == audit,
          "verified batch changed the capture audit");
  const auto& descriptor = verified.descriptor();
  require(descriptorHasLoad(descriptor, 0.17)
              && descriptorHasLoad(descriptor, 0.31),
          "verified descriptor lost native load/capacitance evidence");
  auto replacement_identity
      = baseline.state.reference_payload.source_identity;
  replacement_identity.session_token
      = descriptor.session_token.value();
  replacement_identity.graph_generation
      = descriptor.target_generation.value();
  require(descriptor.reference_payload.instances
              && descriptor.reference_payload.referenced_library_cells
              && descriptor.reference_payload.cell_pin_capacitances
              && descriptor.reference_payload.clock_owner_by_node
              && descriptor.reference_payload.source_identity
                     == std::optional<ValueReplacement<
                         csr::CsrSnapshotIdentity>>{ValueReplacement<
                         csr::CsrSnapshotIdentity>{
                         baseline.state.reference_payload.source_identity,
                         replacement_identity}}
              && !descriptor.reference_payload.schema_version
              && !descriptor.reference_payload.analysis_context
              && !descriptor.reference_payload.clocks
              && !descriptor.reference_payload.parasitics,
          "verified descriptor reference delta is not narrowly scoped");
}

void checkFullWithRecordCannotMintTargetedSuccess()
{
  Baseline baseline = makeBaseline();
  DesignEditPlan plan = bufferInsertionPlan(
      baseline.fixture.database(), baseline.baseline, kGeneration,
      editTestCreationToken());
  plan.edits.front().rc_inputs
      = ParasiticRcInputs{ParasiticEditPolicy::kFullWithRecord};
  PendingStructuredBatch pending
      = prepareBuffer(baseline, std::move(plan));
  CandidateBoundCapture rejected = read(
      baseline, baseline.fixture.database(), pending,
      pending.capturePlan());
  require(rejected.error() == NarrowCaptureError::kUnsupportedPolicy
              && !rejected.success() && rejected.audit() != nullptr
              && !rejected.audit()->full_export_guard_active
              && rejected.audit()->native_full_graph_exports == 0
              && rejected.audit()->native_full_reference_exports == 0,
          "FullWithRecord/default-zero audit minted targeted success");
}

}  // namespace

int main()
{
  checkStableReresolutionAndBoundaryCanaries();
  checkFullWithRecordCannotMintTargetedSuccess();
  return 0;
}
