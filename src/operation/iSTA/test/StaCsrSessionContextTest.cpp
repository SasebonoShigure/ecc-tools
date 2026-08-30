// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "CsrSnapshotObservation.hpp"
#include "Database.hpp"
#include "StaCsrSessionContext.hpp"

namespace sta_csr_context_allocation_failpoint {
namespace {

struct AllocationState
{
  bool armed{false};
  bool injected{false};
};

thread_local AllocationState state;

void beforeAllocation()
{
  if (!state.armed)
    return;
  state.armed = false;
  state.injected = true;
  throw std::bad_alloc{};
}

}  // namespace

void arm() noexcept
{
  state = AllocationState{true, false};
}

[[nodiscard]] bool finish() noexcept
{
  const bool injected = state.injected;
  state = AllocationState{};
  return injected;
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

}  // namespace sta_csr_context_allocation_failpoint

void* operator new(std::size_t size)
{
  return sta_csr_context_allocation_failpoint::allocate(size);
}

void* operator new[](std::size_t size)
{
  return sta_csr_context_allocation_failpoint::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return sta_csr_context_allocation_failpoint::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  try {
    return sta_csr_context_allocation_failpoint::allocate(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  return sta_csr_context_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  return sta_csr_context_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return sta_csr_context_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  try {
    return sta_csr_context_allocation_failpoint::allocateAligned(size, static_cast<std::size_t>(alignment));
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

namespace ista {

class StaCsrSessionContextTestAccess
{
 public:
  static StaCsrSessionSubstate substate(const StaCsrSessionContext& context) noexcept { return context.substate(); }
  static StaCsrCaptureAttempt capture(StaCsrSessionContext& context, const Database& database) { return context.capture(database); }
  static StaCsrReferenceAttempt computeReference(StaCsrSessionContext& context) { return context.computeReference(); }
  static StaCsrScatterAttempt scatter(StaCsrSessionContext& context, Database& database, const csr::CsrTimingResult& result)
  {
    return context.scatter(database, result);
  }
  static void release(StaCsrSessionContext& context, std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept { context.release(detached_out); }
  static bool copyObservation(StaCsrSessionContext& context, std::unique_ptr<csr::CsrSnapshotObservation>* detached_out)
  {
    return context.copyObservation(detached_out);
  }
};

}  // namespace ista

namespace {

using namespace ista;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

csr::StaAnalysisContext validContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}

void populateSingleton(Database& database)
{
  Pin pin;
  pin.set_full_name("only");
  pin.set_pin_name("only");
  pin.set_net_name("shared");
  pin.set_direction(PinDirection::kInput);
  pin.set_is_port(true);
  database.get_pin_map().emplace("only", std::move(pin));
  database.get_timing_point_map().emplace("only", TimingPoint{});

  Net net;
  net.set_net_name("shared");
  net.set_pin_name_list({"only"});
  net.set_driver_pin_list({"only"});
  net.set_load_pin_list({});
  net.set_driver_pin("only");
  database.get_net_map().emplace("shared", std::move(net));
  database.get_start_point_list() = {"only"};
}

void populateMalformed(Database& database)
{
  populateSingleton(database);
  Arc arc;
  arc.set_arc_name("invalid-native-kind");
  arc.set_source_pin("only");
  arc.set_sink_pin("only");
  arc.set_owner_name("shared");
  arc.set_type(static_cast<ArcType>(0xff));
  database.get_arc_list().push_back(std::move(arc));
  database.get_outgoing_arc_list_map()["only"].push_back(0);
  database.get_incoming_arc_list_map()["only"].push_back(0);
}
void requireInvalidReference(const StaCsrReferenceAttempt& attempt, const std::string& name)
{
  const auto* failure = std::get_if<StaCsrSessionOperationFailure>(&attempt.value);
  require(failure != nullptr, name + " did not return local invalid-state");
  require(failure->code == StaCsrSessionOperationCode::kInvalidState && failure->writes_applied == 0, name + " returned the wrong invalid-state payload");
}

void requireInvalidScatter(const StaCsrScatterAttempt& attempt, const std::string& name)
{
  require(attempt.value.scatter_result.code == csr::CsrScatterCode::kInvalidState, name + " did not return invalid_state");
  require(attempt.value.scatter_result.writes_applied == 0, name + " reported writes");
  require(!attempt.value.scatter_result.invalidation_reason.has_value(), name + " reported an invalidation reason");
}

void testConstructorAndAwaitingState()
{
  bool zero_rejected = false;
  try {
    StaCsrSessionContext invalid(validContext(), 0);
  } catch (const std::invalid_argument&) {
    zero_rejected = true;
  }
  require(zero_rejected, "zero session token was accepted");

  bool context_rejected = false;
  try {
    StaCsrSessionContext invalid(csr::StaAnalysisContext{}, 1);
  } catch (const std::invalid_argument&) {
    context_rejected = true;
  }
  require(context_rejected, "invalid analysis context was accepted");

  StaCsrSessionContext context(validContext(), 29);
  require(context.token() == 29, "session token accessor drifted");
  require(context.analysisContext() == validContext(), "analysis context accessor drifted");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kAwaitingCapture, "new context did not start awaiting capture");

  std::unique_ptr<csr::CsrSnapshotObservation> observation;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &observation), "awaiting observation read was rejected");
  require(!observation, "awaiting observation read returned a value");

  requireInvalidReference(StaCsrSessionContextTestAccess::computeReference(context), "awaiting reference");
  Database database;
  requireInvalidScatter(StaCsrSessionContextTestAccess::scatter(context, database, csr::CsrTimingResult{}), "awaiting scatter");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kAwaitingCapture, "invalid awaiting operations changed the substate");
}

void testCaptureOneShotAndRelease()
{
  Database database;
  populateSingleton(database);
  StaCsrSessionContext context(validContext(), 31);

  const StaCsrCaptureAttempt first = StaCsrSessionContextTestAccess::capture(context, database);
  const auto* capture = std::get_if<csr::CsrCaptureResult>(&first.value);
  require(capture != nullptr && !capture->error.has_value(), "singleton capture did not return exact typed success");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReady, "successful capture did not publish ready");

  std::unique_ptr<csr::CsrSnapshotObservation> captured;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &captured) && captured != nullptr, "ready capture omitted its observation");
  require(captured->state == csr::CsrSnapshotState::kReady && captured->identity.session_token == context.token(),
          "captured observation state or token drifted");
  require(captured->counts == csr::CsrSnapshotCounts{1, 0, 1}, "singleton capture counts drifted");

  StaCsrReferenceAttempt reference = StaCsrSessionContextTestAccess::computeReference(context);
  const auto* computation = std::get_if<csr::CsrComputationResult>(&reference.value);
  require(computation != nullptr, "ready reference did not return the exact outer computation result");
  const auto* timing = std::get_if<csr::CsrTimingResult>(&computation->outcome);
  require(timing != nullptr && timing->source_identity == captured->identity, "singleton reference did not return matching timing");

  std::unique_ptr<csr::CsrSnapshotObservation> before_second;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &before_second) && before_second != nullptr, "pre-second-capture observation read failed");
  const StaCsrCaptureAttempt second = StaCsrSessionContextTestAccess::capture(context, database);
  const auto* failure = std::get_if<StaCsrSessionOperationFailure>(&second.value);
  require(failure != nullptr, "second capture did not return local invalid-state");
  require(failure->code == StaCsrSessionOperationCode::kInvalidState && failure->writes_applied == 0,
          "second capture returned the wrong invalid-state payload");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReady, "second capture changed the ready substate");

  std::unique_ptr<csr::CsrSnapshotObservation> after_second;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &after_second) && after_second != nullptr, "post-second-capture observation read failed");
  require(*before_second == *after_second, "second capture changed the committed observation");

  std::unique_ptr<csr::CsrSnapshotObservation> released;
  StaCsrSessionContextTestAccess::release(context, &released);
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReleased, "release did not publish released substate");
  require(released != nullptr && released->state == csr::CsrSnapshotState::kReleased, "ready release did not detach a released observation");
  require(released->identity.session_token == context.token(), "detached observation token drifted");

  const csr::CsrSnapshotObservation* released_address = released.get();
  const csr::CsrSnapshotObservation released_value = *released;
  StaCsrSessionContextTestAccess::release(context, &released);
  require(released.get() == released_address && *released == released_value, "double release overwrote caller-owned observation");

  std::unique_ptr<csr::CsrSnapshotObservation> after_release;
  require(!StaCsrSessionContextTestAccess::copyObservation(context, &after_release), "released observation read was accepted");
  require(!after_release, "released observation read wrote its out parameter");
  requireInvalidReference(StaCsrSessionContextTestAccess::computeReference(context), "released reference");
  requireInvalidScatter(StaCsrSessionContextTestAccess::scatter(context, database, csr::CsrTimingResult{}), "released scatter");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReleased, "invalid released operations changed the substate");
}

void testCaptureFailureConsumesOneShot()
{
  Database malformed;
  populateMalformed(malformed);
  StaCsrSessionContext context(validContext(), 41);

  const StaCsrCaptureAttempt first = StaCsrSessionContextTestAccess::capture(context, malformed);
  const auto* capture = std::get_if<csr::CsrCaptureResult>(&first.value);
  require(capture != nullptr && capture->error == csr::CsrCaptureCode::kMalformedTopology, "malformed Database did not return exact typed capture failure");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kCaptureFailed, "typed capture failure did not publish capture_failed");

  std::unique_ptr<csr::CsrSnapshotObservation> before_second;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &before_second), "capture_failed observation read was rejected");
  require(!before_second, "capture_failed observation read returned a value");

  Database valid;
  populateSingleton(valid);
  const StaCsrCaptureAttempt second = StaCsrSessionContextTestAccess::capture(context, valid);
  const auto* failure = std::get_if<StaCsrSessionOperationFailure>(&second.value);
  require(failure != nullptr && failure->code == StaCsrSessionOperationCode::kInvalidState && failure->writes_applied == 0,
          "capture after typed failure did not return exact local invalid-state");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kCaptureFailed, "capture after typed failure changed substate");

  std::unique_ptr<csr::CsrSnapshotObservation> after_second;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &after_second), "post-second capture_failed observation read was rejected");
  require(!after_second, "capture after typed failure published an observation");

  std::unique_ptr<csr::CsrSnapshotObservation> released;
  StaCsrSessionContextTestAccess::release(context, &released);
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReleased, "capture_failed release did not publish released");
  require(!released, "capture_failed release returned an observation");
  StaCsrSessionContextTestAccess::release(context, &released);
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReleased && !released, "double capture_failed release was not a no-op");
}
void testCaptureThrowConsumesOneShot()
{
  Database database;
  populateSingleton(database);
  database.get_timing_order_list() = {"throw-poison"};
  const auto database_before = database.get_timing_order_list();
  StaCsrSessionContext context(validContext(), 43);

  bool caught_bad_alloc = false;
  bool injected = false;
  sta_csr_context_allocation_failpoint::arm();
  try {
    static_cast<void>(StaCsrSessionContextTestAccess::capture(context, database));
    injected = sta_csr_context_allocation_failpoint::finish();
  } catch (const std::bad_alloc&) {
    injected = sta_csr_context_allocation_failpoint::finish();
    caught_bad_alloc = true;
  } catch (...) {
    static_cast<void>(sta_csr_context_allocation_failpoint::finish());
    throw;
  }

  require(injected && caught_bad_alloc, "capture did not propagate the injected allocation failure");
  require(database.get_timing_order_list() == database_before, "capture throw changed Database");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kCaptureFailed,
          "capture throw did not permanently publish capture_failed");
  std::unique_ptr<csr::CsrSnapshotObservation> before_second;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &before_second), "capture-throw observation read was rejected");
  require(!before_second, "capture throw partially published an observation");

  const StaCsrCaptureAttempt second = StaCsrSessionContextTestAccess::capture(context, database);
  const auto* failure = std::get_if<StaCsrSessionOperationFailure>(&second.value);
  require(failure != nullptr && failure->code == StaCsrSessionOperationCode::kInvalidState && failure->writes_applied == 0,
          "capture after throw did not return exact local invalid-state");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kCaptureFailed, "capture after throw changed capture_failed substate");
  std::unique_ptr<csr::CsrSnapshotObservation> after_second;
  require(StaCsrSessionContextTestAccess::copyObservation(context, &after_second), "post-second capture-throw observation read was rejected");
  require(!after_second, "capture after throw published an observation");
  require(database.get_timing_order_list() == database_before, "capture after throw changed Database");

  std::unique_ptr<csr::CsrSnapshotObservation> released;
  StaCsrSessionContextTestAccess::release(context, &released);
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReleased && !released,
          "capture-throw release did not publish released/null");
  StaCsrSessionContextTestAccess::release(context, &released);
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kReleased && !released, "double capture-throw release was not a no-op");
}

void testInvalidOutParametersDoNotMutate()
{
  StaCsrSessionContext context(validContext(), 37);
  auto sentinel = std::make_unique<csr::CsrSnapshotObservation>();
  csr::CsrSnapshotObservation* sentinel_address = sentinel.get();
  require(!StaCsrSessionContextTestAccess::copyObservation(context, &sentinel), "nonempty copy out parameter was accepted");
  require(sentinel.get() == sentinel_address, "nonempty copy out parameter was overwritten");
  require(!StaCsrSessionContextTestAccess::copyObservation(context, nullptr), "null copy out parameter was accepted");

  StaCsrSessionContextTestAccess::release(context, &sentinel);
  require(sentinel.get() == sentinel_address, "nonempty release out parameter was overwritten");
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kAwaitingCapture, "invalid release out parameter changed the substate");
  StaCsrSessionContextTestAccess::release(context, nullptr);
  require(StaCsrSessionContextTestAccess::substate(context) == StaCsrSessionSubstate::kAwaitingCapture, "null release out parameter changed the substate");
}

}  // namespace

int main()
{
  try {
    testConstructorAndAwaitingState();
    testCaptureOneShotAndRelease();
    testCaptureFailureConsumesOneShot();
    testCaptureThrowConsumesOneShot();
    testInvalidOutParametersDoNotMutate();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
