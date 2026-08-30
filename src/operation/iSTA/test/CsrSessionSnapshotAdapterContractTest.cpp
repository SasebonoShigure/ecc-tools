// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrSessionSnapshotAdapter.hpp"
#include "CsrSnapshotObservationTransition.hpp"
#include "Database.hpp"

namespace csr_session_allocation_failpoint {
namespace {

struct AllocationState
{
  bool enabled{false};
  bool inject_failure{false};
  std::size_t failure_ordinal{0};
  std::size_t allocation_count{0};
};

thread_local AllocationState state;

void beforeAllocation()
{
  if (!state.enabled)
    return;
  const std::size_t ordinal = state.allocation_count++;
  if (state.inject_failure && ordinal == state.failure_ordinal)
    throw std::bad_alloc{};
}

}  // namespace

void armCountOnly() noexcept
{
  state = AllocationState{true, false, 0, 0};
}

void armFailure(std::size_t ordinal) noexcept
{
  state = AllocationState{true, true, ordinal, 0};
}

[[nodiscard]] std::size_t disable() noexcept
{
  const std::size_t result = state.allocation_count;
  state = AllocationState{};
  return result;
}

[[nodiscard]] void* allocate(std::size_t size)
{
  beforeAllocation();
  if (void* allocation = std::malloc(size == 0 ? 1 : size))
    return allocation;
  throw std::bad_alloc{};
}

[[nodiscard]] void* allocateAligned(std::size_t size, std::size_t alignment)
{
  beforeAllocation();
  void* allocation = nullptr;
  if (::posix_memalign(&allocation, alignment, size == 0 ? 1 : size) == 0)
    return allocation;
  throw std::bad_alloc{};
}

}  // namespace csr_session_allocation_failpoint

void* operator new(std::size_t size)
{
  return csr_session_allocation_failpoint::allocate(size);
}

void* operator new[](std::size_t size)
{
  return csr_session_allocation_failpoint::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return csr_session_allocation_failpoint::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return csr_session_allocation_failpoint::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  return csr_session_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  return csr_session_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return csr_session_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return csr_session_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* allocation) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept
{
  std::free(allocation);
}

void operator delete(void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

void operator delete[](void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
  std::free(allocation);
}

namespace {

using namespace ista::csr;

constexpr std::uint64_t kSessionToken = 17;
constexpr std::uint64_t kSingletonWrites = 22;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

StaAnalysisContext validContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}

void populateSingleton(ista::Database& database)
{
  ista::Pin pin;
  pin.set_full_name("only");
  pin.set_pin_name("only");
  pin.set_net_name("shared");
  pin.set_direction(ista::PinDirection::kInput);
  pin.set_is_port(true);
  database.get_pin_map().emplace("only", std::move(pin));
  database.get_timing_point_map().emplace("only", ista::TimingPoint{});

  ista::Net net;
  net.set_net_name("shared");
  net.set_pin_name_list({"only"});
  net.set_driver_pin_list({"only"});
  net.set_load_pin_list({});
  net.set_driver_pin("only");
  database.get_net_map().emplace("shared", std::move(net));
  database.get_start_point_list() = {"only"};
}

CsrSnapshotObservation releasedFrom(const CsrSnapshotObservation& observation)
{
  CsrOptionalObservation before{observation};
  CsrOptionalObservationResult result = reduceSnapshotObservation(before, ReleaseAction{});
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result)) {
    const std::string detail = errors->empty() ? "no violation" : errors->front().field + ": " + errors->front().message;
    throw std::runtime_error("release reduction failed: " + detail);
  }
  CsrOptionalObservation reduced = std::move(std::get<CsrOptionalObservation>(result));
  require(reduced.has_value(), "release reduction returned no observation");
  return std::move(*reduced);
}

std::unique_ptr<CsrCommittedSessionSnapshot> takeSnapshot(CsrSessionSnapshotBuildResult result, const std::string& name)
{
  require(std::holds_alternative<std::unique_ptr<CsrCommittedSessionSnapshot>>(result), name + " returned a typed capture failure");
  auto snapshot = std::move(std::get<std::unique_ptr<CsrCommittedSessionSnapshot>>(result));
  require(snapshot != nullptr, name + " returned a null snapshot");
  require(snapshot->active_observation != nullptr && snapshot->released_observation != nullptr, name + " omitted an observation");
  return snapshot;
}

struct BuildCase
{
  ista::Database database;
  std::vector<std::string> database_before;
};

struct SessionCase
{
  ista::Database database;
  StaAnalysisContext current_context{validContext()};
  std::unique_ptr<CsrCommittedSessionSnapshot> snapshot;
  CsrTimingResult result;
  CsrSnapshotObservation active_before;
  CsrSnapshotObservation released_before;
  const CsrSnapshotObservation* active_address{nullptr};
  const CsrSnapshotObservation* released_address{nullptr};
  std::vector<std::string> database_before;
};

std::unique_ptr<BuildCase> makeBuildCase()
{
  auto fixture = std::make_unique<BuildCase>();
  populateSingleton(fixture->database);
  fixture->database.get_timing_order_list() = {"build-poison"};
  fixture->database_before = fixture->database.get_timing_order_list();
  return fixture;
}

void recordBefore(SessionCase& fixture)
{
  require(fixture.snapshot != nullptr && fixture.snapshot->active_observation != nullptr && fixture.snapshot->released_observation != nullptr,
          "fixture has no committed observation pair");
  fixture.active_before = *fixture.snapshot->active_observation;
  fixture.released_before = *fixture.snapshot->released_observation;
  fixture.active_address = fixture.snapshot->active_observation.get();
  fixture.released_address = fixture.snapshot->released_observation.get();
  fixture.database_before = fixture.database.get_timing_order_list();
}

std::unique_ptr<SessionCase> makeReferenceCase()
{
  auto fixture = std::make_unique<SessionCase>();
  populateSingleton(fixture->database);
  fixture->snapshot = takeSnapshot(buildCsrSessionSnapshot(fixture->database, validContext(), kSessionToken), "singleton build");
  fixture->database.get_timing_order_list() = {"timing-order-poison"};
  recordBefore(*fixture);
  return fixture;
}

enum class ScatterMode
{
  kSuccess,
  kRejection,
  kInvalidation
};

std::unique_ptr<SessionCase> makeScatterCase(ScatterMode mode)
{
  auto fixture = makeReferenceCase();
  CsrComputationResult computation = computeCsrSessionReference(*fixture->snapshot);
  auto* timing = std::get_if<CsrTimingResult>(&computation.outcome);
  require(timing != nullptr, "singleton reference did not return timing");
  fixture->result = std::move(*timing);
  if (mode == ScatterMode::kRejection)
    fixture->result.schema_version = "invalid-schema";
  if (mode == ScatterMode::kInvalidation)
    fixture->current_context.rc_corner = "different_rc";
  recordBefore(*fixture);
  return fixture;
}

void requireSessionUnchanged(const SessionCase& fixture, const std::string& name)
{
  require(fixture.database.get_timing_order_list() == fixture.database_before, name + ": Database timing order changed");
  require(fixture.snapshot->active_observation.get() == fixture.active_address, name + ": active observation pointer changed");
  require(fixture.snapshot->released_observation.get() == fixture.released_address, name + ": released observation pointer changed");
  require(*fixture.snapshot->active_observation == fixture.active_before, name + ": active observation changed");
  require(*fixture.snapshot->released_observation == fixture.released_before, name + ": released observation changed");
}

template <typename MakeCase, typename Invoke, typename VerifyFailure, typename VerifySuccess>
void exerciseAllocationOrdinals(const std::string& name, MakeCase make_case, Invoke invoke, VerifyFailure verify_failure, VerifySuccess verify_success)
{
  std::size_t allocation_count = 0;
  {
    auto fixture = make_case();
    csr_session_allocation_failpoint::armCountOnly();
    try {
      auto outcome = invoke(*fixture);
      allocation_count = csr_session_allocation_failpoint::disable();
      verify_success(*fixture, outcome, name + " count-only");
    } catch (...) {
      static_cast<void>(csr_session_allocation_failpoint::disable());
      throw;
    }
  }
  require(allocation_count > 0, name + " performed no observable allocations");
  require(allocation_count < std::numeric_limits<std::size_t>::max(), name + " cannot represent total+1");

  for (std::size_t ordinal = 0; ordinal < allocation_count; ++ordinal) {
    auto fixture = make_case();
    const std::string ordinal_name = name + " allocation ordinal " + std::to_string(ordinal);
    bool caught_bad_alloc = false;
    bool returned = false;
    std::size_t observed_allocations = 0;
    std::exception_ptr unexpected_exception;
    csr_session_allocation_failpoint::armFailure(ordinal);
    try {
      auto outcome = invoke(*fixture);
      observed_allocations = csr_session_allocation_failpoint::disable();
      static_cast<void>(outcome);
      returned = true;
    } catch (const std::bad_alloc&) {
      observed_allocations = csr_session_allocation_failpoint::disable();
      caught_bad_alloc = true;
    } catch (...) {
      observed_allocations = csr_session_allocation_failpoint::disable();
      unexpected_exception = std::current_exception();
    }

    verify_failure(*fixture, ordinal_name);
    if (unexpected_exception)
      std::rethrow_exception(unexpected_exception);
    require(caught_bad_alloc && !returned, ordinal_name + ": did not propagate std::bad_alloc");
    require(observed_allocations == ordinal + 1, ordinal_name + ": failure ordinal was not reached exactly");
  }

  auto fixture = make_case();
  const std::size_t success_ordinal = allocation_count + 1;
  std::size_t observed_allocations = 0;
  try {
    csr_session_allocation_failpoint::armFailure(success_ordinal);
    auto outcome = invoke(*fixture);
    observed_allocations = csr_session_allocation_failpoint::disable();
    verify_success(*fixture, outcome, name + " total+1");
  } catch (...) {
    static_cast<void>(csr_session_allocation_failpoint::disable());
    throw;
  }
  require(observed_allocations == allocation_count, name + " total+1 allocation count changed");
}

void verifyBuildSuccess(const BuildCase& fixture, const CsrSessionSnapshotBuildResult& result, const std::string& name)
{
  require(fixture.database.get_timing_order_list() == fixture.database_before, name + ": build changed Database");
  const auto* candidate = std::get_if<std::unique_ptr<CsrCommittedSessionSnapshot>>(&result);
  require(candidate != nullptr && *candidate != nullptr, name + ": build did not return a snapshot");
  const auto& snapshot = **candidate;
  require(snapshot.input.source_identity.session_token == kSessionToken, name + ": session token drifted");
  require(snapshot.active_observation->state == CsrSnapshotState::kReady, name + ": active observation is not ready");
  require(snapshot.active_observation->counts == CsrSnapshotCounts{1, 0, 1}, name + ": singleton counts drifted");
  require(std::isfinite(snapshot.active_observation->timing_ms.export_ms) && snapshot.active_observation->timing_ms.export_ms >= 0.0,
          name + ": export duration is not finite and nonnegative");
  require(*snapshot.released_observation == releasedFrom(*snapshot.active_observation), name + ": released twin drifted");
}

void testBuildAllocationAtomicity()
{
  exerciseAllocationOrdinals(
      "build", makeBuildCase, [](BuildCase& fixture) { return buildCsrSessionSnapshot(fixture.database, validContext(), kSessionToken); },
      [](const BuildCase& fixture, const std::string& name) {
        require(fixture.database.get_timing_order_list() == fixture.database_before, name + ": failed build changed Database");
      },
      verifyBuildSuccess);
}

void verifyReferenceSuccess(const SessionCase& fixture, const CsrComputationResult& computation, const std::string& name)
{
  const auto* timing = std::get_if<CsrTimingResult>(&computation.outcome);
  require(timing != nullptr, name + ": reference did not return timing");
  require(timing->source_identity == fixture.snapshot->input.source_identity, name + ": reference identity drifted");
  require(fixture.database.get_timing_order_list() == fixture.database_before, name + ": reference changed Database");
  require(fixture.snapshot->active_observation.get() != fixture.active_address, name + ": active observation was mutated in place");
  require(fixture.snapshot->released_observation.get() != fixture.released_address, name + ": released observation was mutated in place");
  require(fixture.snapshot->active_observation->state == CsrSnapshotState::kReady, name + ": active observation left ready");
  require(fixture.snapshot->active_observation->timing_ms.reference_ms.has_value(), name + ": reference duration was not published");
  const double duration = *fixture.snapshot->active_observation->timing_ms.reference_ms;
  require(std::isfinite(duration) && duration >= 0.0, name + ": reference duration is invalid");
  require(*fixture.snapshot->released_observation == releasedFrom(*fixture.snapshot->active_observation), name + ": released twin was not published");
}

void testReferenceAllocationAtomicity()
{
  exerciseAllocationOrdinals(
      "reference", makeReferenceCase, [](SessionCase& fixture) { return computeCsrSessionReference(*fixture.snapshot); },
      [](const SessionCase& fixture, const std::string& name) { requireSessionUnchanged(fixture, name); }, verifyReferenceSuccess);
}

void requireScatterResult(const CsrTimingResultScatterAttempt& attempt, CsrScatterCode code, std::uint64_t writes, std::optional<CsrInvalidationReason> reason,
                          const std::string& name)
{
  require(attempt.scatter_result.code == code, name + ": scatter code drifted");
  require(attempt.scatter_result.writes_applied == writes, name + ": scatter write count drifted");
  require(attempt.scatter_result.invalidation_reason == reason, name + ": invalidation reason drifted");
}

void verifyScatterSuccess(const SessionCase& fixture, const CsrTimingResultScatterAttempt& attempt, ScatterMode mode, const std::string& name)
{
  const auto& active = *fixture.snapshot->active_observation;
  require(fixture.snapshot->active_observation.get() != fixture.active_address, name + ": active observation was mutated in place");
  require(fixture.snapshot->released_observation.get() != fixture.released_address, name + ": released observation was mutated in place");
  require(active.timing_ms.scatter_ms.has_value() && std::isfinite(*active.timing_ms.scatter_ms) && *active.timing_ms.scatter_ms >= 0.0,
          name + ": scatter duration is invalid");

  if (mode == ScatterMode::kSuccess) {
    requireScatterResult(attempt, CsrScatterCode::kSuccess, kSingletonWrites, std::nullopt, name);
    require(fixture.database.get_timing_order_list() == std::vector<std::string>{"only"}, name + ": Database timing result was not published");
    require(active.state == CsrSnapshotState::kReady && !active.last_rejection && !active.last_invalidation, name + ": success observation event drifted");
  } else if (mode == ScatterMode::kRejection) {
    requireScatterResult(attempt, CsrScatterCode::kSchemaMismatch, 0, std::nullopt, name);
    require(fixture.database.get_timing_order_list() == fixture.database_before, name + ": rejection changed Database");
    require(active.state == CsrSnapshotState::kReady && active.last_rejection.has_value() && !active.last_invalidation,
            name + ": rejection observation event drifted");
    require(active.last_rejection->reason == CsrScatterCode::kSchemaMismatch && active.last_rejection->writes_applied == 0 && !active.last_rejection->source,
            name + ": rejection payload drifted");
  } else {
    requireScatterResult(attempt, CsrScatterCode::kSnapshotInvalidated, 0, CsrInvalidationReason::kCornerChanged, name);
    require(fixture.database.get_timing_order_list() == fixture.database_before, name + ": invalidation changed Database");
    require(active.state == CsrSnapshotState::kInvalidated && !active.last_rejection && active.last_invalidation.has_value(),
            name + ": invalidation observation event drifted");
    require(active.last_invalidation->reason == CsrInvalidationReason::kCornerChanged && active.last_invalidation->writes_applied == 0,
            name + ": invalidation payload drifted");
  }
  require(*fixture.snapshot->released_observation == releasedFrom(active), name + ": released twin was not jointly published");
}

void testScatterAllocationAtomicity(ScatterMode mode, const std::string& name)
{
  exerciseAllocationOrdinals(
      name, [mode] { return makeScatterCase(mode); },
      [](SessionCase& fixture) {
        return scatterCsrSessionTimingResult(fixture.database, *fixture.snapshot, fixture.result, fixture.current_context, kSessionToken);
      },
      [](const SessionCase& fixture, const std::string& ordinal_name) { requireSessionUnchanged(fixture, ordinal_name); },
      [mode](const SessionCase& fixture, const CsrTimingResultScatterAttempt& attempt, const std::string& success_name) {
        verifyScatterSuccess(fixture, attempt, mode, success_name);
      });
}

void testInvalidStateHasNoObservationEvent()
{
  auto fixture = makeScatterCase(ScatterMode::kSuccess);
  const CsrTimingResultScatterAttempt attempt
      = scatterCsrSessionTimingResult(fixture->database, *fixture->snapshot, fixture->result, fixture->current_context, kSessionToken + 1);
  requireScatterResult(attempt, CsrScatterCode::kInvalidState, 0, std::nullopt, "invalid-state scatter");
  require(attempt.current_fingerprints == CsrCurrentFingerprints{} && !attempt.current_capture_error, "invalid-state scatter formed current-capture details");
  requireSessionUnchanged(*fixture, "invalid-state scatter");
}

void testReleaseIsMonotonicAndIdempotent()
{
  auto fixture = makeScatterCase(ScatterMode::kSuccess);
  const CsrSnapshotObservation ready_before = *fixture->snapshot->active_observation;

  releaseCsrSessionSnapshot(*fixture->snapshot);
  require(fixture->snapshot->active_observation->state == CsrSnapshotState::kReleased, "release did not publish released state");
  require(*fixture->snapshot->active_observation == releasedFrom(ready_before), "release did not publish the prebuilt released twin");

  const CsrSnapshotObservation released_once = *fixture->snapshot->active_observation;
  const CsrSnapshotObservation* released_address = fixture->snapshot->active_observation.get();
  releaseCsrSessionSnapshot(*fixture->snapshot);
  require(fixture->snapshot->active_observation.get() == released_address, "double release replaced the released observation");
  require(*fixture->snapshot->active_observation == released_once, "double release changed the released observation");

  bool rejected = false;
  try {
    static_cast<void>(computeCsrSessionReference(*fixture->snapshot));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "released snapshot accepted another reference computation");
  require(fixture->snapshot->active_observation.get() == released_address, "rejected computation changed the released observation address");
  require(*fixture->snapshot->active_observation == released_once, "rejected computation changed the released observation");
}

void testMalformedCommittedSnapshotFailsWithoutPublication()
{
  auto fixture = makeReferenceCase();
  fixture->snapshot->released_observation.reset();
  const CsrSnapshotObservation active_before = *fixture->snapshot->active_observation;
  const CsrSnapshotObservation* active_address = fixture->snapshot->active_observation.get();

  bool rejected = false;
  try {
    static_cast<void>(computeCsrSessionReference(*fixture->snapshot));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "malformed committed snapshot was accepted");
  require(fixture->snapshot->active_observation.get() == active_address, "malformed snapshot failure replaced the active observation");
  require(*fixture->snapshot->active_observation == active_before, "malformed snapshot failure changed the active observation");
}

}  // namespace

int main()
{
  try {
    testBuildAllocationAtomicity();
    testReferenceAllocationAtomicity();
    testScatterAllocationAtomicity(ScatterMode::kSuccess, "scatter success");
    testScatterAllocationAtomicity(ScatterMode::kRejection, "scatter rejection");
    testScatterAllocationAtomicity(ScatterMode::kInvalidation, "scatter invalidation");
    testInvalidStateHasNoObservationEvent();
    testReleaseIsMonotonicAndIdempotent();
    testMalformedCommittedSnapshotFailsWithoutPublication();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
