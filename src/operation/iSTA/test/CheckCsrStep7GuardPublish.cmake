# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

function(require_step7_guard_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 7 guard/publish file: ${FILE_PATH}")
  endif()
endfunction()

function(read_step7_guard_file OUTPUT FILE_PATH)
  require_step7_guard_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(require_step7_guard_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
  set(REMAINDER "${${CONTENT_VARIABLE}}")
  set(ACTUAL 0)
  string(LENGTH "${TOKEN}" TOKEN_LENGTH)
  while(TRUE)
    string(FIND "${REMAINDER}" "${TOKEN}" POSITION)
    if(POSITION EQUAL -1)
      break()
    endif()
    math(EXPR OFFSET "${POSITION} + ${TOKEN_LENGTH}")
    string(SUBSTRING "${REMAINDER}" ${OFFSET} -1 REMAINDER)
    math(EXPR ACTUAL "${ACTUAL} + 1")
  endwhile()
  if(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 7 guard detector rejected ${DESCRIPTION}: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

function(require_step7_guard_absent CONTENT_VARIABLE TOKEN DESCRIPTION)
  string(FIND "${${CONTENT_VARIABLE}}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR
            "Step 7 guard detector rejected ${DESCRIPTION}: prohibited token ${TOKEN}")
  endif()
endfunction()

function(require_step7_order CONTENT_VARIABLE DESCRIPTION)
  set(CURSOR 0)
  foreach(TOKEN IN LISTS ARGN)
    string(SUBSTRING "${${CONTENT_VARIABLE}}" ${CURSOR} -1 TAIL)
    string(FIND "${TAIL}" "${TOKEN}" RELATIVE)
    if(RELATIVE EQUAL -1)
      message(FATAL_ERROR
              "Step 7 guard detector rejected ${DESCRIPTION}: missing/out-of-order token ${TOKEN}")
    endif()
    string(LENGTH "${TOKEN}" TOKEN_LENGTH)
    math(EXPR CURSOR "${CURSOR} + ${RELATIVE} + ${TOKEN_LENGTH}")
  endforeach()
endfunction()

if(DEFINED CSR_STEP7_GUARD_DETECTOR_MODE)
  if(CSR_STEP7_GUARD_DETECTOR_MODE STREQUAL source)
    read_step7_guard_file(DETECTOR_CONTENT "${CSR_STEP7_GUARD_DETECTOR_FILE}")
    string(REPLACE "|" ";" DETECTOR_TOKENS "${CSR_STEP7_GUARD_DETECTOR_TOKENS}")
    require_step7_order(DETECTOR_CONTENT "ordered-source fixture" ${DETECTOR_TOKENS})
  elseif(CSR_STEP7_GUARD_DETECTOR_MODE STREQUAL allocation)
    execute_process(
        COMMAND "${NM_EXECUTABLE}" -uC "${CSR_STEP7_GUARD_DETECTOR_FILE}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE DETECTOR_CONTENT ERROR_VARIABLE ERROR)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 7 guard detector cannot inspect object: ${ERROR}")
    endif()
    string(FIND "${DETECTOR_CONTENT}" "operator new" ALLOCATION)
    if(NOT ALLOCATION EQUAL -1)
      message(FATAL_ERROR "Step 7 guard detector rejected allocation symbol")
    endif()
  else()
    message(FATAL_ERROR "unknown Step 7 guard detector mode")
  endif()
  return()
endif()

foreach(REQUIRED_VARIABLE
        ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR NINJA_EXECUTABLE
        CXX_COMPILER NM_EXECUTABLE CSR_GENERATOR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

function(run_step7_guard_detector MODE FILE_PATH TOKENS EXPECT_SUCCESS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              "-DCSR_STEP7_GUARD_DETECTOR_MODE=${MODE}"
              "-DCSR_STEP7_GUARD_DETECTOR_FILE=${FILE_PATH}"
              "-DCSR_STEP7_GUARD_DETECTOR_TOKENS=${TOKENS}"
              "-DNM_EXECUTABLE=${NM_EXECUTABLE}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 7 guard safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 7 guard detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 7 guard poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

set(GUARD_DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step7-guard-detector")
file(REMOVE_RECURSE "${GUARD_DETECTOR_DIR}")
file(MAKE_DIRECTORY "${GUARD_DETECTOR_DIR}")
file(WRITE "${GUARD_DETECTOR_DIR}/source-safe.txt"
     "sentinel\nlifecycle-first\noperation-lock\nlifecycle-second\ncontext-token\n")
file(WRITE "${GUARD_DETECTOR_DIR}/source-poison.txt"
     "lifecycle-first\nsentinel\noperation-lock\nlifecycle-second\ncontext-token\n")
set(ORDER_TOKENS
    "sentinel|lifecycle-first|operation-lock|lifecycle-second|context-token")
run_step7_guard_detector(
    source "${GUARD_DETECTOR_DIR}/source-safe.txt" "${ORDER_TOKENS}" TRUE)
run_step7_guard_detector(
    source "${GUARD_DETECTOR_DIR}/source-poison.txt" "${ORDER_TOKENS}" FALSE)
file(WRITE "${GUARD_DETECTOR_DIR}/allocation-safe.cpp"
     "void allocationSafe() noexcept {}\n")
file(WRITE "${GUARD_DETECTOR_DIR}/allocation-poison.cpp"
     "#include <new>\nvoid* allocationPoison() { return ::operator new(16); }\n")
foreach(FIXTURE safe poison)
  execute_process(
      COMMAND "${CXX_COMPILER}" -std=c++20 -c
              "${GUARD_DETECTOR_DIR}/allocation-${FIXTURE}.cpp"
              -o "${GUARD_DETECTOR_DIR}/allocation-${FIXTURE}.o"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "cannot compile Step 7 allocation fixture: ${OUT}${ERR}")
  endif()
endforeach()
run_step7_guard_detector(
    allocation "${GUARD_DETECTOR_DIR}/allocation-safe.o" unused TRUE)
run_step7_guard_detector(
    allocation "${GUARD_DETECTOR_DIR}/allocation-poison.o" unused FALSE)
file(REMOVE_RECURSE "${GUARD_DETECTOR_DIR}")
if(EXISTS "${GUARD_DETECTOR_DIR}")
  message(FATAL_ERROR "Step 7 guard detector cleanup failed")
endif()

set(INTERFACE_DIR "${ISTA_SOURCE_ROOT}/interface")
set(ADAPTER_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr")
set(CORE_DIR "${ISTA_SOURCE_ROOT}/source/module/timing_csr")
get_filename_component(CHILD_SRC_ROOT "${ISTA_SOURCE_ROOT}/../.." ABSOLUTE)
set(PY_DIR "${CHILD_SRC_ROOT}/interface/python/py_ista")
set(LIFECYCLE_HEADER "${INTERFACE_DIR}/StaLifecycleGuard.hpp")
set(LIFECYCLE_SOURCE "${INTERFACE_DIR}/StaLifecycleGuard.cpp")
set(SESSION_HEADER "${INTERFACE_DIR}/IncrementalStaSession.hpp")
set(SESSION_SOURCE "${INTERFACE_DIR}/IncrementalStaSession.cpp")
set(CONTEXT_HEADER "${INTERFACE_DIR}/StaCsrSessionContext.hpp")
set(CONTEXT_SOURCE "${INTERFACE_DIR}/StaCsrSessionContext.cpp")
set(INTERFACE_HEADER "${INTERFACE_DIR}/STAInterface.hpp")
set(OPERATIONS_SOURCE "${INTERFACE_DIR}/STAInterfaceOperations.cpp")
set(ADAPTER_SOURCE "${ADAPTER_DIR}/CsrSessionSnapshotAdapter.cpp")
set(PUBLISH_SOURCE "${ADAPTER_DIR}/TimingResultScatterPublish.cpp")
set(IDENTITY_SOURCE "${CORE_DIR}/CsrSnapshotIdentityValidator.cpp")
set(BINDING_SOURCE "${PY_DIR}/py_ista.cpp")
set(PROBE_SOURCE "${CSR_TEST_ROOT}/projection/StaCsrProjectionProbe.cpp")
set(LIFECYCLE_TEST "${CSR_TEST_ROOT}/StaCsrSessionLifecycleTest.cpp")
set(DATA_MANAGER_SOURCE "${ISTA_SOURCE_ROOT}/source/data_manager/DataManager.cpp")
set(TIMING_CHARACTERIZER_SOURCE "${ISTA_SOURCE_ROOT}/source/module/timing_characterizer/TimingCharacterizer.cpp")
foreach(FILE
        "${LIFECYCLE_HEADER}" "${LIFECYCLE_SOURCE}" "${SESSION_HEADER}"
        "${SESSION_SOURCE}" "${CONTEXT_HEADER}" "${CONTEXT_SOURCE}"
        "${INTERFACE_HEADER}" "${OPERATIONS_SOURCE}" "${ADAPTER_SOURCE}"
        "${PUBLISH_SOURCE}" "${IDENTITY_SOURCE}" "${BINDING_SOURCE}"
        "${DATA_MANAGER_SOURCE}" "${TIMING_CHARACTERIZER_SOURCE}"
        "${PROBE_SOURCE}" "${LIFECYCLE_TEST}")
  require_step7_guard_file("${FILE}")
endforeach()
read_step7_guard_file(LIFECYCLE_HEADER_CONTENT "${LIFECYCLE_HEADER}")
read_step7_guard_file(LIFECYCLE_CONTENT "${LIFECYCLE_SOURCE}")
read_step7_guard_file(SESSION_HEADER_CONTENT "${SESSION_HEADER}")
read_step7_guard_file(SESSION_CONTENT "${SESSION_SOURCE}")
read_step7_guard_file(CONTEXT_HEADER_CONTENT "${CONTEXT_HEADER}")
read_step7_guard_file(CONTEXT_CONTENT "${CONTEXT_SOURCE}")
read_step7_guard_file(INTERFACE_HEADER_CONTENT "${INTERFACE_HEADER}")
read_step7_guard_file(OPERATIONS_CONTENT "${OPERATIONS_SOURCE}")
read_step7_guard_file(ADAPTER_CONTENT "${ADAPTER_SOURCE}")
read_step7_guard_file(PUBLISH_CONTENT "${PUBLISH_SOURCE}")
read_step7_guard_file(IDENTITY_CONTENT "${IDENTITY_SOURCE}")
read_step7_guard_file(BINDING_CONTENT "${BINDING_SOURCE}")
read_step7_guard_file(PROBE_CONTENT "${PROBE_SOURCE}")
read_step7_guard_file(LIFECYCLE_TEST_CONTENT "${LIFECYCLE_TEST}")
read_step7_guard_file(DATA_MANAGER_CONTENT "${DATA_MANAGER_SOURCE}")
read_step7_guard_file(TIMING_CHARACTERIZER_CONTENT "${TIMING_CHARACTERIZER_SOURCE}")

foreach(TOKEN
        "class StaSessionTokenAllocator"
        "explicit StaSessionTokenAllocator(std::uint64_t initial = 1) noexcept"
        "[[nodiscard]] std::optional<std::uint64_t> allocate() noexcept"
        "std::atomic<std::uint64_t> _next")
  require_step7_guard_count(LIFECYCLE_HEADER_CONTENT "${TOKEN}" 1 "allocator public contract ${TOKEN}")
endforeach()
require_step7_order(
    LIFECYCLE_CONTENT "MAX sticky allocator"
    "_next.load(std::memory_order_relaxed)"
    "while (current != 0)"
    "std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1"
    "_next.compare_exchange_weak"
    "return current"
    "return std::nullopt")
require_step7_guard_count(
    LIFECYCLE_CONTENT "if (token == 0)" 3 "token-zero lease depth")
require_step7_guard_count(
    LIFECYCLE_CONTENT "static StaSessionTokenAllocator allocator;" 1
    "production process singleton")
foreach(PROHIBITED reset setNext set_next)
  require_step7_guard_absent(
      LIFECYCLE_HEADER_CONTENT "${PROHIBITED}" "production allocator reset API")
endforeach()

require_step7_order(
    SESSION_CONTENT "context-before-token initialization"
    "validateAnalysisContextIntrinsic(analysis_context)"
    "resolveStage1StaExecution(_request)"
    "productionStaSessionTokenAllocator().allocate()"
    "_token.store(*allocated_token, std::memory_order_release)"
    "std::make_unique<StaCsrSessionContext>"
    "_operations->initialize")
require_step7_guard_count(
    SESSION_HEADER_CONTENT "std::atomic<std::uint64_t> _token{0};" 1
    "session token sentinel")
require_step7_guard_count(
    SESSION_CONTENT "productionStaSessionTokenAllocator().allocate()" 1
    "allocator sole session call")
require_step7_order(
    BINDING_CONTENT "tokenless factory"
    "createIncrementalStaSession"
    "return std::make_unique<ista::IncrementalStaSession>(parseStaExecutionRequest(request));"
    "initializeIncrementalStaSession")
require_step7_guard_count(
    BINDING_CONTENT "productionStaSessionTokenAllocator" 0
    "binding factory allocator calls")

require_step7_order(
    CONTEXT_CONTENT "one-shot capture outer commit"
    "_substate = StaCsrSessionSubstate::kCaptureFailed"
    "buildCsrSessionSnapshot"
    "auto snapshot_candidate"
    "StaCsrCaptureAttempt success_attempt"
    "_substate = StaCsrSessionSubstate::kReady"
    "_snapshot.swap(snapshot_candidate)"
    "return success_attempt")
require_step7_guard_count(
    CONTEXT_CONTENT "static_assert(std::is_nothrow_move_constructible_v<StaCsr" 3
    "named wrapper nothrow move proofs")
require_step7_guard_count(
    CONTEXT_CONTENT "static_assert(std::is_nothrow_destructible_v<StaCsr" 3
    "named wrapper nothrow destruction proofs")
require_step7_guard_count(
    CONTEXT_CONTENT "static_assert(noexcept(StaCsr" 3
    "named wrapper no-throw construction proofs")
require_step7_order(
    CONTEXT_CONTENT "release handoff"
    "releaseCsrSessionSnapshot(*_snapshot)"
    "*detached_out = std::move(_snapshot->active_observation)"
    "_substate = StaCsrSessionSubstate::kReleased")

require_step7_guard_count(
    IDENTITY_CONTENT "CsrTimingResultViolationClass::kSchema" 3
    "malformed identity schema classification")
require_step7_guard_absent(
    IDENTITY_CONTENT "CsrTimingResultViolationClass::kSession"
    "malformed identity session classification")
require_step7_guard_absent(
    IDENTITY_CONTENT "CsrTimingResultViolationClass::kSource"
    "malformed identity source classification")

foreach(ALLOCATION_TOKEN
        "make_unique" "new " "emplace" "push_back" "reduceSnapshotObservation"
        "makeReadySnapshotObservation")
  require_step7_guard_absent(
      PUBLISH_CONTENT "${ALLOCATION_TOKEN}" "no-fail publisher source")
endforeach()
foreach(FUNCTION_TOKEN
        "publishPreparedTimingResult(PreparedTimingResult& prepared) noexcept"
        "publishPreparedSessionObservationPair(CsrCommittedSessionSnapshot& snapshot, PreparedSessionObservationPair& prepared) noexcept"
        "publishPreparedTimingResultAndSessionObservationPair(PreparedTimingResult& timing_result, CsrCommittedSessionSnapshot& snapshot,"
        "publishReleasedSessionObservation(CsrCommittedSessionSnapshot& snapshot) noexcept")
  require_step7_guard_count(
      PUBLISH_CONTENT "${FUNCTION_TOKEN}" 1 "no-fail publisher function")
endforeach()
require_step7_order(
    PUBLISH_CONTENT "joint timing/observation publish"
    "publishPreparedTimingResult(timing_result)"
    "publishPreparedSessionObservationPair(snapshot, observation_pair)")
require_step7_order(
    PUBLISH_CONTENT "monotonic released observation"
    "snapshot.active_observation->state == CsrSnapshotState::kReleased"
    "snapshot.active_observation.swap(snapshot.released_observation)")
require_step7_guard_count(
    PUBLISH_CONTENT "snapshot.active_observation.swap(snapshot.released_observation)" 1
    "single monotonic release swap")
require_step7_order(
    ADAPTER_CONTENT "reference observation publish then plain return"
    "detail::publishPreparedSessionObservationPair(snapshot, observation_pair)"
    "return result;")
require_step7_order(
    ADAPTER_CONTENT "joint scatter publish then plain return"
    "detail::publishPreparedTimingResultAndSessionObservationPair(envelope.prepared, snapshot, observation_pair)"
    "return success_attempt;")
require_step7_guard_count(
    ADAPTER_CONTENT "detail::publishPreparedSessionObservationPair(snapshot, observation_pair);" 3
    "adapter prepared observation publishes")
require_step7_guard_count(
    ADAPTER_CONTENT "detail::publishPreparedTimingResultAndSessionObservationPair(" 1
    "adapter joint success publish")
require_step7_guard_count(
    ADAPTER_CONTENT "detail::publishReleasedSessionObservation(snapshot);" 1
    "adapter monotonic release façade")

require_step7_order(
    OPERATIONS_CONTENT "use guard double-check"
    "if (operationSentinelActive())"
    "std::scoped_lock lifecycle_lock(_lifecycle_mutex)"
    "std::unique_lock operation_lock(_operation_mutex)"
    "std::optional<OperationGuard> guard"
    "std::scoped_lock lifecycle_lock(_lifecycle_mutex)"
    "context->token() != owner.token"
    "return guard")
require_step7_order(
    OPERATIONS_CONTENT "close reservation and handoff before cleanup"
    "if (operationSentinelActive())"
    "std::optional<ClosePendingReservation> reservation"
    "reservation.emplace(this, owner)"
    "std::unique_lock operation_lock(_operation_mutex)"
    "context->token() != owner.token"
    "context->release(released_out)"
    "_lifecycle_operations->close()"
    "_lifecycle.completeClose(result.succeeded)"
    "reservation->commit()")
require_step7_guard_count(
    OPERATIONS_CONTENT "context->token() != owner.token" 2
    "use/close context-owner token checks")
foreach(RESERVATION_TOKEN
        "ClosePendingReservation::~ClosePendingReservation() noexcept"
        "_interface->_close_pending = StaOwner::none();"
        "ClosePendingReservation::commit() noexcept")
  string(FIND "${OPERATIONS_CONTENT}" "${RESERVATION_TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "missing close reservation rollback/commit token ${RESERVATION_TOKEN}")
  endif()
endforeach()
require_step7_order(
    SESSION_CONTENT "closed immutable observation getter"
    "if (state() == StaSessionState::kClosed)"
    "_released_observation"
    "observationOwnershipError(*_released_observation)"
    "std::make_unique<csr::CsrSnapshotObservation>(*_released_observation)")
require_step7_order(
    BINDING_CONTENT "binding session/observation ownership check"
    "session.snapshotObservation()"
    "session.sessionToken()"
    "observation->identity.session_token != *token")
string(FIND "${INTERFACE_HEADER_CONTENT}" "class STAInterface\n{" PUBLIC_CLASS_START)
if(PUBLIC_CLASS_START EQUAL -1)
  message(FATAL_ERROR "missing STAInterface public API block")
endif()
string(SUBSTRING "${INTERFACE_HEADER_CONTENT}" ${PUBLIC_CLASS_START} -1 PUBLIC_CLASS_TAIL)
string(FIND "${PUBLIC_CLASS_TAIL}" "\n private:" PUBLIC_CLASS_END)
if(PUBLIC_CLASS_END EQUAL -1)
  message(FATAL_ERROR "unterminated STAInterface public API block")
endif()
string(SUBSTRING "${PUBLIC_CLASS_TAIL}" 0 ${PUBLIC_CLASS_END} PUBLIC_API_BLOCK)
string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*\\(" PUBLIC_METHOD_MATCHES
       "${PUBLIC_API_BLOCK}")
string(REPLACE "(" "" ACTUAL_PUBLIC_METHODS "${PUBLIC_METHOD_MATCHES}")
set(EXPECTED_PUBLIC_METHODS
    getInst destroyInst
    initSTA runSTA extractLib destroySTA updateTiming writeSDF reportTiming
    createClock setPropagatedClock getPorts getClocks
    initializeSession runSessionBaseline closeSession
    copySessionCsrSnapshotObservation computeSessionCsrReferenceForTesting
    scatterSessionCsrTimingResultForTesting lifecycleStateForTesting
    setLifecycleOperationsForTesting)
if(NOT "${ACTUAL_PUBLIC_METHODS}" STREQUAL "${EXPECTED_PUBLIC_METHODS}")
  message(FATAL_ERROR
          "STAInterface exact public API differs: [${ACTUAL_PUBLIC_METHODS}]")
endif()

require_step7_guard_count(
    DATA_MANAGER_CONTENT "STAI." 2 "DataManager private-friend caller total")
require_step7_guard_count(
    DATA_MANAGER_CONTENT "STAI.input(" 1 "DataManager input private-friend caller")
require_step7_guard_count(
    DATA_MANAGER_CONTENT "STAI.output(" 1 "DataManager output private-friend caller")
require_step7_guard_count(
    TIMING_CHARACTERIZER_CONTENT "STAI." 1
    "TimingCharacterizer private-friend caller total")
require_step7_guard_count(
    TIMING_CHARACTERIZER_CONTENT "STAI.writeLib(" 1
    "TimingCharacterizer writeLib private-friend caller")

foreach(FRIEND
        "friend class DataManager;"
        "friend class TimingCharacterizer;"
        "friend class StaCsrSessionProbeAccess;"
        "friend class StaInterfaceLifecycleTestAccess;")
  require_step7_guard_count(
      INTERFACE_HEADER_CONTENT "${FRIEND}" 1 "private friend allow-list")
endforeach()
require_step7_guard_count(
    INTERFACE_HEADER_CONTENT "inspectSessionDatabaseForTesting(" 1
    "private Database inspection declaration")
require_step7_guard_count(
    OPERATIONS_CONTENT "STAInterface::inspectSessionDatabaseForTesting(" 1
    "guarded Database inspection definition")
require_step7_guard_count(
    PROBE_CONTENT "class StaCsrSessionProbeAccess" 1
    "probe private friend seam")
require_step7_guard_count(
    PROBE_CONTENT "inspectSessionDatabaseForTesting(token, callback)" 1
    "probe guarded Database callsite")
require_step7_guard_count(
    LIFECYCLE_TEST_CONTENT "class StaInterfaceLifecycleTestAccess" 1
    "lifecycle private friend seam")
require_step7_guard_count(
    PROBE_CONTENT "STADM.getDatabase(" 0
    "probe direct Database access")

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 7 guard/publish gate requires Ninja")
endif()
require_step7_guard_file("${NINJA_EXECUTABLE}")
set(PUBLISH_OBJECT
    "${CSR_BUILD_DIR}/src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatterPublish.cpp.o")
require_step7_guard_file("${PUBLISH_OBJECT}")
execute_process(
    COMMAND "${NM_EXECUTABLE}" -uC "${PUBLISH_OBJECT}"
    RESULT_VARIABLE NM_RESULT OUTPUT_VARIABLE NM_OUTPUT ERROR_VARIABLE NM_ERROR)
if(NOT NM_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 7 publisher symbols: ${NM_ERROR}")
endif()
string(FIND "${NM_OUTPUT}" "operator new" PUBLISH_ALLOCATION)
if(NOT PUBLISH_ALLOCATION EQUAL -1)
  message(FATAL_ERROR "Step 7 no-fail publisher has an allocation symbol")
endif()
execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps
            "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatterPublish.cpp.o"
    RESULT_VARIABLE DEPS_RESULT OUTPUT_VARIABLE DEPS ERROR_VARIABLE DEPS_ERROR)
if(NOT DEPS_RESULT EQUAL 0 OR NOT DEPS MATCHES "\\(VALID\\)"
   OR NOT DEPS MATCHES "TimingResultScatterPublish.cpp")
  message(FATAL_ERROR "Step 7 publisher depfile missing/stale: ${DEPS}${DEPS_ERROR}")
endif()

message(STATUS "CSR Step 7 token, guard, ownership, and no-fail publish inventory passed")
