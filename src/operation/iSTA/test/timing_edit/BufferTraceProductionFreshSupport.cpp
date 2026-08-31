// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceProductionFreshSupport.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "BufferFixedTraceGenerator.hpp"
#include "GreedyBufferCandidateSelector.hpp"
#include "BufferTraceReplayer.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "IncrementalStaSession.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeBufferObservationExtractor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "ProductionFreshSessionLoader.hpp"

namespace ista::buffer_trace::test_support {
namespace {

[[noreturn]] void reject(std::string message)
{
  throw FreshBaselineError(FreshBaselineFailureCode::kBaselineCaptureFailed,
                           "derived timing freshness: " + std::move(message));
}

[[noreturn]] void rejectFirstEmpty(std::string message)
{
  throw FreshBaselineError(FreshBaselineFailureCode::kGuardedWorkFailed,
                           "first empty selection: " + std::move(message));
}

std::unique_ptr<csr::CsrCommittedSessionSnapshot> snapshotFor(
    const Database& database,
    const timing_edit::oracle::ProductionFreshSessionView& view)
{
  csr::CsrSessionSnapshotBuildResult built = csr::buildCsrSessionSnapshot(
      database, view.reference_input.analysis_context, view.session_token);
  auto* snapshot
      = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&built);
  if (snapshot == nullptr || *snapshot == nullptr) {
    reject("CSR snapshot build rejected generation zero");
  }
  return std::move(*snapshot);
}

csr::CsrComputationResult compute(
    csr::CsrCommittedSessionSnapshot& snapshot)
{
  csr::CsrComputationResult result
      = csr::computeCsrSessionReference(snapshot);
  if (std::get_if<csr::CsrTimingResult>(&result.outcome) == nullptr) {
    reject("CPU/full reference did not return a timing result");
  }
  return result;
}

timing_edit::CommittedEditIdentity identityFor(
    const Database& database,
    const timing_edit::oracle::ProductionFreshSessionView& view)
{
  return timing_edit::NativeTimingEditPreparer::captureCurrentIdentity(
      database, timing_edit::TransactionGeneration{0},
      view.reference_input.analysis_context,
      timing_edit::TimingEditSessionToken{view.session_token});
}

bool sameReferenceInput(const csr::CsrReferenceInput& lhs,
                        const csr::CsrReferenceInput& rhs)
{
  // Canonical bytes cover every logical field; identity is intentionally a
  // separate envelope and is therefore compared explicitly here.
  return lhs.source_identity == rhs.source_identity
         && csr::canonicalReferenceInputBytes(lhs)
                == csr::canonicalReferenceInputBytes(rhs);
}

}  // namespace

DerivedTimingFreshnessFunctionalReceipt proveDerivedTimingFreshness(
    const timing_edit::oracle::ProductionFreshSessionView& view,
    const timing_edit::CommittedEditIdentity& initial_identity)
{
  csr::CsrCpuReferenceBackend cpu;
  const csr::CsrComputationResult baseline_cpu
      = cpu.compute(view.reference_input);
  if (std::get_if<csr::CsrTimingResult>(&baseline_cpu.outcome) == nullptr) {
    reject("baseline CPU/full reference is unsupported");
  }
  const auto& baseline_points = view.database.get_timing_point_map();
  if (baseline_points.empty()) {
    reject("generation-zero Database has no timing point");
  }
  const auto point = std::find_if(
      baseline_points.begin(), baseline_points.end(), [](const auto& item) {
        return std::isfinite(item.second.get_arrival());
      });
  if (point == baseline_points.end()) {
    reject("generation-zero Database has no finite-arrival timing point");
  }
  const auto& [point_name, baseline_point] = *point;
  timing_edit::NativeDatabaseCandidate candidate
      = timing_edit::NativeDatabaseCandidate::create(view.database);
  if (!candidate.database().get_timing_point_map().empty()) {
    reject("detached candidate retained derived timing state");
  }
  auto [injected, inserted]
      = candidate.database().get_timing_point_map().emplace(point_name,
                                                             baseline_point);
  if (!inserted) {
    reject("could not inject the detached timing point tamper");
  }
  const double baseline_arrival = injected->second.get_arrival();
  const double tampered_arrival = baseline_arrival == 0.0 ? 1.0 : 0.0;
  injected->second.set_arrival(tampered_arrival);
  if (injected->second.get_arrival() == baseline_arrival) {
    reject("arrival tamper did not change the detached candidate");
  }

  timing_edit::InstalledNativeDatabase installed
      = timing_edit::NativeCandidateExecutor::installAndExecute(
          view.authority, std::move(candidate));
  DerivedTimingFreshnessFunctionalReceipt receipt;
  receipt.exercised = true;
  receipt.timing_point = point_name;
  receipt.baseline_arrival = baseline_arrival;
  receipt.tampered_arrival = tampered_arrival;
  receipt.tamper_observed = true;
  if (!std::isfinite(receipt.baseline_arrival)
      || !std::isfinite(receipt.tampered_arrival)) {
    reject("baseline or tampered arrival is not finite");
  }
  try {
    auto recovered_snapshot = snapshotFor(installed.database(), view);
    const csr::CsrComputationResult recovered_cpu
        = compute(*recovered_snapshot);
    const auto recovered_point
        = installed.database().get_timing_point_map().find(point_name);
    if (recovered_point == installed.database().get_timing_point_map().end()) {
      reject("full rerun did not rebuild the tampered timing point");
    }
    receipt.recovered_arrival = recovered_point->second.get_arrival();
    if (!std::isfinite(receipt.recovered_arrival)) {
      reject("full rerun rebuilt a non-finite arrival");
    }
    receipt.csr_node_count = recovered_snapshot->input.nodes.size();
    receipt.csr_arc_count = recovered_snapshot->input.arcs.size();
    receipt.rerun_input_exact
        = sameReferenceInput(recovered_snapshot->input, view.reference_input);
    receipt.rerun_cpu_exact = recovered_cpu == baseline_cpu;
    receipt.rerun_identity_exact
        = identityFor(installed.database(), view) == initial_identity;
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    if (!installed.rollback()) {
      reject("full rerun failure also failed exact rollback");
    }
    std::rethrow_exception(failure);
  }
  if (!installed.rollback()) {
    reject("InstalledNativeDatabase exact rollback failed");
  }

  receipt.rollback_owner_restored = &STADM.getDatabase() == &view.database;
  auto rollback_snapshot = snapshotFor(STADM.getDatabase(), view);
  const csr::CsrComputationResult rollback_cpu = compute(*rollback_snapshot);
  receipt.rollback_input_exact
      = sameReferenceInput(rollback_snapshot->input, view.reference_input);
  receipt.rollback_cpu_exact = rollback_cpu == baseline_cpu;
  receipt.rollback_identity_exact
      = identityFor(STADM.getDatabase(), view) == initial_identity;
  receipt.session_remained_baseline_executed
      = view.session.state() == StaSessionState::kBaselineExecuted;
  receipt.authority_remained_active = view.authority.active();
  if (receipt.recovered_arrival != receipt.baseline_arrival
      || !receipt.rerun_input_exact || !receipt.rerun_cpu_exact
      || !receipt.rerun_identity_exact || !receipt.rollback_owner_restored
      || !receipt.rollback_input_exact || !receipt.rollback_cpu_exact
      || !receipt.rollback_identity_exact
      || !receipt.session_remained_baseline_executed
      || !receipt.authority_remained_active) {
    reject("full rerun or rollback differed from exact generation zero");
  }
  return receipt;
}

bool proveFirstEmptyGenerationZero(
    const timing_edit::oracle::ProductionFreshSessionView& view,
    const timing_edit::TimingEditCoordinator& coordinator,
    const timing_edit::CommittedEditIdentity& initial_identity)
{
  const timing_edit::TransactionGeneration zero{0};
  const csr::CsrCommittedSessionSnapshot* host = coordinator.hostSnapshot();
  timing_edit::TimingEditResolverState rebound;
  try {
    rebound = timing_edit::TimingEditResolverState::bindBaseline(
        STADM.getDatabase());
    const SelectionProfile profile
        = frozenSelectionProfile(ProfileId::kEngineeringSmoke);
    std::vector<BufferCandidateObservation> observations
        = NativeBufferObservationExtractor::extract(
            STADM.getDatabase(), rebound, profile);
    GreedySelectionResult selected = GreedyBufferCandidateSelector{}.select(
        {.profile = profile,
         .case_id = "stage4-first-empty",
         .seed = 42,
         .batch_ordinal = 1,
         .max_acceptances = profile.max_total_insertions,
         .candidates = observations,
         .reserved_creation_tokens = {},
         .reserved_materialized_names = {}});
    const auto* selection = std::get_if<GreedySelection>(&selected);
    const bool all_negative = selection != nullptr
        && selection->accepted.empty()
        && selection->receipt.accepted_tokens.empty()
        && selection->receipt.evaluations.size() == observations.size()
        && std::ranges::all_of(
            selection->receipt.evaluations, [](const auto& evaluation) {
              return evaluation.decision == CandidateDecision::kRejected
                     && evaluation.rejection_reason
                            == CandidateRejectionReason::kNonPositiveGain
                     && evaluation.numeric
                     && evaluation.numeric->candidate_load_pf
                            == evaluation.numeric->buffer_capacitance_pf
                     && evaluation.numeric->delta_capacitance_pf == 0.0
                     && evaluation.numeric->benefit_ns == 0.0
                     && evaluation.numeric->gain_ns < +0.0;
            });
    if (observations.size() != 4 || !all_negative)
      rejectFirstEmpty("no-gain fixture did not produce four exact negative-gain evaluations");
  } catch (const std::exception& error) {
    rejectFirstEmpty(std::string("observation proof failed: ") + error.what());
  }
  const bool exact
      = coordinator.state() == timing_edit::TimingEditCoordinatorState::kIdle
        && coordinator.committedIdentity() == initial_identity
        && coordinator.cumulativeTrace().batches.empty()
        && coordinator.deviceState().generation == zero
        && coordinator.commitMarker().generation == zero
        && host != nullptr
        && sameReferenceInput(host->input, view.reference_input)
        && coordinator.resolverState() == rebound
        && &STADM.getDatabase() == &view.database
        && identityFor(STADM.getDatabase(), view) == initial_identity
        && view.session.state() == StaSessionState::kBaselineExecuted
        && view.authority.active();
  if (!exact) {
    rejectFirstEmpty("generation-zero coordinator, native Database, CSR, "
                     "resolver, session, or authority state changed");
  }
  return true;
}

void rethrowTypedLoaderFailure(std::size_t callback_count,
                               std::exception_ptr failure)
{
  try {
    std::rethrow_exception(failure);
  } catch (const FreshBaselineError&) {
    throw;
  } catch (const BufferReplayError&) {
    throw;
  } catch (const BufferGenerationError&) {
    throw;
  } catch (const std::exception& error) {
    throw FreshBaselineError(
        callback_count == 0
            ? FreshBaselineFailureCode::kArtifactMaterializationRejected
            : FreshBaselineFailureCode::kGuardedWorkFailed,
        std::string("production fresh loader threw: ") + error.what());
  } catch (...) {
    throw FreshBaselineError(
        callback_count == 0
            ? FreshBaselineFailureCode::kArtifactMaterializationRejected
            : FreshBaselineFailureCode::kGuardedWorkFailed,
        "production fresh loader threw a non-standard exception");
  }
}

}  // namespace ista::buffer_trace::test_support
