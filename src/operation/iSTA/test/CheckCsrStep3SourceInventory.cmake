# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

foreach(REQUIRED_VARIABLE ISTA_SOURCE_ROOT CSR_TEST_ROOT)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

function(read_required_file OUTPUT FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 3 source-inventory file: ${FILE_PATH}")
  endif()
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(require_regex_count CONTENT_VARIABLE REGEX EXPECTED DESCRIPTION)
  string(REGEX MATCHALL "${REGEX}" MATCHES "${${CONTENT_VARIABLE}}")
  list(LENGTH MATCHES ACTUAL)
  if(NOT ACTUAL EQUAL EXPECTED)
    message(
        FATAL_ERROR
        "${DESCRIPTION} count mismatch: actual=${ACTUAL}, expected=${EXPECTED}"
    )
  endif()
endfunction()


set(SESSION_BRIDGE_PUBLIC
    "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/CsrSessionSnapshotAdapter.hpp")
set(SESSION_BRIDGE_INTERNAL
    "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/CsrSessionSnapshotAdapterInternal.hpp")
set(SESSION_BRIDGE_SOURCE
    "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/CsrSessionSnapshotAdapter.cpp")

set(FIFO_PROHIBITED_LOCAL_TOKENS
    "#include <deque>"
    "#include <queue>"
    "std::queue"
    "std::deque"
    "std::priority_queue"
    "in_degree"
    "indegree"
    "ready_nodes"
    "ready_queue"
)

function(check_step3_session_bridge PUBLIC_FILE INTERNAL_FILE SOURCE_FILE)
  foreach(BRIDGE_FILE "${PUBLIC_FILE}" "${INTERNAL_FILE}" "${SOURCE_FILE}")
    read_required_file(BRIDGE_CONTENT "${BRIDGE_FILE}")
    foreach(PROHIBITED_TOKEN IN LISTS FIFO_PROHIBITED_LOCAL_TOKENS)
      string(FIND "${BRIDGE_CONTENT}" "${PROHIBITED_TOKEN}" TOKEN_POSITION)
      if(NOT TOKEN_POSITION EQUAL -1)
        message(FATAL_ERROR
                "Step 3 bridge detector rejected local Kahn token ${PROHIBITED_TOKEN}: ${BRIDGE_FILE}")
      endif()
    endforeach()
  endforeach()
  read_required_file(BRIDGE_PUBLIC_CONTENT "${PUBLIC_FILE}")
  read_required_file(BRIDGE_INTERNAL_CONTENT "${INTERNAL_FILE}")
  read_required_file(BRIDGE_SOURCE_CONTENT "${SOURCE_FILE}")
  foreach(HEADER_CONTENT_VARIABLE BRIDGE_PUBLIC_CONTENT BRIDGE_INTERNAL_CONTENT)
    string(REGEX MATCHALL "replayFifoLevelization\\(" HEADER_CALLS
           "${${HEADER_CONTENT_VARIABLE}}")
    list(LENGTH HEADER_CALLS HEADER_CALL_COUNT)
    if(NOT HEADER_CALL_COUNT EQUAL 0)
      message(FATAL_ERROR
              "Step 3 bridge detector rejected replay call outside bridge source")
    endif()
  endforeach()
  string(REGEX MATCHALL "replayFifoLevelization\\(" SOURCE_CALLS
         "${BRIDGE_SOURCE_CONTENT}")
  list(LENGTH SOURCE_CALLS SOURCE_CALL_COUNT)
  if(NOT SOURCE_CALL_COUNT EQUAL 1)
    message(FATAL_ERROR
            "Step 3 bridge detector rejected bridge source replay count: ${SOURCE_CALL_COUNT}")
  endif()
endfunction()

if(DEFINED CSR_STEP3_BRIDGE_DETECTOR_MODE)
  foreach(REQUIRED_DETECTOR_VARIABLE
          CSR_STEP3_BRIDGE_PUBLIC CSR_STEP3_BRIDGE_INTERNAL CSR_STEP3_BRIDGE_SOURCE)
    if(NOT DEFINED ${REQUIRED_DETECTOR_VARIABLE})
      message(FATAL_ERROR "${REQUIRED_DETECTOR_VARIABLE} is required")
    endif()
  endforeach()
  check_step3_session_bridge("${CSR_STEP3_BRIDGE_PUBLIC}"
                             "${CSR_STEP3_BRIDGE_INTERNAL}"
                             "${CSR_STEP3_BRIDGE_SOURCE}")
  return()
endif()

function(run_step3_bridge_detector NAME PUBLIC_FILE INTERNAL_FILE SOURCE_FILE EXPECT_SUCCESS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              -DISTA_SOURCE_ROOT=unused -DCSR_TEST_ROOT=unused
              -DCSR_STEP3_BRIDGE_DETECTOR_MODE=TRUE
              "-DCSR_STEP3_BRIDGE_PUBLIC=${PUBLIC_FILE}"
              "-DCSR_STEP3_BRIDGE_INTERNAL=${INTERNAL_FILE}"
              "-DCSR_STEP3_BRIDGE_SOURCE=${SOURCE_FILE}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 3 bridge safe self-test ${NAME} failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 3 bridge detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 3 bridge poison self-test ${NAME} failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

if(DEFINED CSR_BUILD_DIR)
  set(STEP3_DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step3-bridge-detector")
  set(STEP3_DETECTOR_PUBLIC "${STEP3_DETECTOR_DIR}/CsrSessionSnapshotAdapter.hpp")
  set(STEP3_DETECTOR_INTERNAL "${STEP3_DETECTOR_DIR}/CsrSessionSnapshotAdapterInternal.hpp")
  set(STEP3_DETECTOR_SOURCE "${STEP3_DETECTOR_DIR}/CsrSessionSnapshotAdapter.cpp")
  file(REMOVE_RECURSE "${STEP3_DETECTOR_DIR}")
  file(MAKE_DIRECTORY "${STEP3_DETECTOR_DIR}")
  file(WRITE "${STEP3_DETECTOR_PUBLIC}" "struct PublicSafe {};\n")
  file(WRITE "${STEP3_DETECTOR_INTERNAL}" "struct InternalSafe {};\n")
  file(WRITE "${STEP3_DETECTOR_SOURCE}" "void bridge() { replayFifoLevelization(input); }\n")
  run_step3_bridge_detector(safe "${STEP3_DETECTOR_PUBLIC}" "${STEP3_DETECTOR_INTERNAL}"
                            "${STEP3_DETECTOR_SOURCE}" TRUE)
  file(APPEND "${STEP3_DETECTOR_PUBLIC}" "std::queue<int> poison;\n")
  run_step3_bridge_detector(public-queue "${STEP3_DETECTOR_PUBLIC}" "${STEP3_DETECTOR_INTERNAL}"
                            "${STEP3_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP3_DETECTOR_PUBLIC}" "struct PublicSafe {};\n")
  file(APPEND "${STEP3_DETECTOR_INTERNAL}" "int in_degree = 0;\n")
  run_step3_bridge_detector(internal-kahn "${STEP3_DETECTOR_PUBLIC}" "${STEP3_DETECTOR_INTERNAL}"
                            "${STEP3_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP3_DETECTOR_INTERNAL}" "struct InternalSafe {};\n")
  file(WRITE "${STEP3_DETECTOR_SOURCE}" "void bridge() {}\n")
  run_step3_bridge_detector(missing-call "${STEP3_DETECTOR_PUBLIC}" "${STEP3_DETECTOR_INTERNAL}"
                            "${STEP3_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP3_DETECTOR_SOURCE}" "void bridge() { replayFifoLevelization(input); }\n")
  file(APPEND "${STEP3_DETECTOR_PUBLIC}" "void wrong() { replayFifoLevelization(input); }\n")
  run_step3_bridge_detector(wrong-file-call "${STEP3_DETECTOR_PUBLIC}" "${STEP3_DETECTOR_INTERNAL}"
                            "${STEP3_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP3_DETECTOR_PUBLIC}" "struct PublicSafe {};\n")
  file(APPEND "${STEP3_DETECTOR_SOURCE}" "int ready_queue = 0;\n")
  run_step3_bridge_detector(local-topology "${STEP3_DETECTOR_PUBLIC}" "${STEP3_DETECTOR_INTERNAL}"
                            "${STEP3_DETECTOR_SOURCE}" FALSE)
  file(REMOVE_RECURSE "${STEP3_DETECTOR_DIR}")
endif()
set(FIFO_CONSUMER_FILES
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrReferenceValidator.cpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrTimingResultValidator.cpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotObservationValidator.cpp"
    "${SESSION_BRIDGE_SOURCE}"
)
check_step3_session_bridge("${SESSION_BRIDGE_PUBLIC}" "${SESSION_BRIDGE_INTERNAL}"
                           "${SESSION_BRIDGE_SOURCE}")
foreach(FIFO_CONSUMER IN LISTS FIFO_CONSUMER_FILES)
  read_required_file(FIFO_CONTENT "${FIFO_CONSUMER}")
  require_regex_count(
      FIFO_CONTENT "replayFifoLevelization\\(" 1
      "FIFO production consumer ${FIFO_CONSUMER}"
  )
  foreach(PROHIBITED_TOKEN IN LISTS FIFO_PROHIBITED_LOCAL_TOKENS)
    string(FIND "${FIFO_CONTENT}" "${PROHIBITED_TOKEN}" TOKEN_POSITION)
    if(NOT TOKEN_POSITION EQUAL -1)
      message(
          FATAL_ERROR
          "FIFO consumer must not contain a local Kahn queue: ${FIFO_CONSUMER}: ${PROHIBITED_TOKEN}"
      )
    endif()
  endforeach()
endforeach()

set(FIFO_HEADER "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrFifoLevelization.hpp")
set(FIFO_SOURCE "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrFifoLevelization.cpp")
read_required_file(FIFO_HEADER_CONTENT "${FIFO_HEADER}")
read_required_file(FIFO_SOURCE_CONTENT "${FIFO_SOURCE}")
require_regex_count(
    FIFO_HEADER_CONTENT "replayFifoLevelization\\(" 1
    "FIFO public declaration"
)
require_regex_count(
    FIFO_SOURCE_CONTENT "replayFifoLevelization\\(" 1
    "FIFO unique implementation"
)
set(FIFO_ALLOWED_CALL_FILES
    ${FIFO_HEADER}
    ${FIFO_SOURCE}
    ${FIFO_CONSUMER_FILES}
)
file(GLOB_RECURSE FIFO_PRODUCTION_FILES LIST_DIRECTORIES FALSE
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr/*.cpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr/*.hpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/*.cpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/*.hpp"
    "${SESSION_BRIDGE_PUBLIC}"
    "${SESSION_BRIDGE_INTERNAL}"
    "${SESSION_BRIDGE_SOURCE}"
)
set(FIFO_TOTAL_CALLS 0)
foreach(FIFO_PRODUCTION_FILE IN LISTS FIFO_PRODUCTION_FILES)
  read_required_file(FIFO_PRODUCTION_CONTENT "${FIFO_PRODUCTION_FILE}")
  string(
      REGEX MATCHALL "replayFifoLevelization\\("
      FIFO_PRODUCTION_CALLS "${FIFO_PRODUCTION_CONTENT}"
  )
  list(LENGTH FIFO_PRODUCTION_CALLS FIFO_PRODUCTION_CALL_COUNT)
  if(FIFO_PRODUCTION_CALL_COUNT GREATER 0)
    list(FIND FIFO_ALLOWED_CALL_FILES "${FIFO_PRODUCTION_FILE}" FIFO_ALLOWED_INDEX)
    if(FIFO_ALLOWED_INDEX EQUAL -1)
      message(
          FATAL_ERROR
          "unregistered FIFO production consumer: ${FIFO_PRODUCTION_FILE}"
      )
    endif()
    math(
        EXPR FIFO_TOTAL_CALLS
        "${FIFO_TOTAL_CALLS} + ${FIFO_PRODUCTION_CALL_COUNT}"
    )
  endif()
endforeach()
if(NOT FIFO_TOTAL_CALLS EQUAL 6)
  message(FATAL_ERROR "FIFO production call inventory must contain exactly 6 sites")
endif()

set(FIFO_BACKEND "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrCpuReferenceBackend.cpp")
read_required_file(FIFO_BACKEND_CONTENT "${FIFO_BACKEND}")
require_regex_count(
    FIFO_BACKEND_CONTENT "&replayFifoLevelization" 1
    "FIFO backend address-take consumer"
)
require_regex_count(
    FIFO_BACKEND_CONTENT "replayFifoLevelization\\(" 0
    "FIFO backend direct-call sites"
)
set(FIFO_TOTAL_BARE_SYMBOLS 0)
foreach(FIFO_PRODUCTION_FILE IN LISTS FIFO_PRODUCTION_FILES)
  read_required_file(FIFO_PRODUCTION_CONTENT "${FIFO_PRODUCTION_FILE}")
  string(REGEX MATCHALL "replayFifoLevelization" FIFO_BARE_SYMBOLS "${FIFO_PRODUCTION_CONTENT}")
  list(LENGTH FIFO_BARE_SYMBOLS FIFO_BARE_COUNT)
  math(EXPR FIFO_TOTAL_BARE_SYMBOLS "${FIFO_TOTAL_BARE_SYMBOLS} + ${FIFO_BARE_COUNT}")
endforeach()
if(NOT FIFO_TOTAL_BARE_SYMBOLS EQUAL 7)
  message(FATAL_ERROR "FIFO production bare-symbol inventory must contain exactly 7 sites")
endif()

set(
    TRANSITION_SOURCE
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotObservationTransition.cpp"
)
read_required_file(TRANSITION_CONTENT "${TRANSITION_SOURCE}")
require_regex_count(
    TRANSITION_CONTENT "action\\.valueless_by_exception\\(\\)" 3
    "valueless action guards"
)
set(GUARD_SNIPPETS
    [=[CsrObservationActionKindResult observationActionKind(const CsrObservationAction& action)
{
  if (action.valueless_by_exception())]=]
    [=[CsrOptionalObservationResult reduceSnapshotObservation(const CsrOptionalObservation& before, const CsrObservationAction& action)
{
  if (action.valueless_by_exception())]=]
    [=[std::vector<CsrContractViolation> validateSnapshotObservationTransition(const CsrOptionalObservation& before, const CsrOptionalObservation& after,
                                                                        const CsrObservationAction& action)
{
  if (action.valueless_by_exception())]=]
)
foreach(GUARD_SNIPPET IN LISTS GUARD_SNIPPETS)
  string(FIND "${TRANSITION_CONTENT}" "${GUARD_SNIPPET}" GUARD_POSITION)
  if(GUARD_POSITION EQUAL -1)
    message(
        FATAL_ERROR
        "each public action entry must check valueless_by_exception before variant access"
    )
  endif()
endforeach()

set(
    TRANSITION_TEST
    "${CSR_TEST_ROOT}/CsrSnapshotTransitionContractTest.cpp"
)
read_required_file(TRANSITION_TEST_CONTENT "${TRANSITION_TEST}")
set(NOTHROW_TYPES
    ReadAction
    ReferenceMeasurementAction
    ScatterRejectionAction
    ScatterSuccessAction
    InvalidationAction
    ReleaseAction
    CsrObservationAction
)
require_regex_count(
    TRANSITION_TEST_CONTENT
    "static_assert\\(std::is_nothrow_move_constructible_v<" 7
    "compile-time nothrow action proofs"
)
foreach(NOTHROW_TYPE IN LISTS NOTHROW_TYPES)
  set(
      ASSERTION
      "static_assert(std::is_nothrow_move_constructible_v<${NOTHROW_TYPE}>);"
  )
  string(FIND "${TRANSITION_TEST_CONTENT}" "${ASSERTION}" ASSERTION_POSITION)
  if(ASSERTION_POSITION EQUAL -1)
    message(
        FATAL_ERROR
        "missing compile-time nothrow action proof for ${NOTHROW_TYPE}"
    )
  endif()
endforeach()

message(STATUS "CSR Step 3 FIFO and valueless source inventory passed")
