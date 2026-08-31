# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

foreach(REQUIRED_VARIABLE ISTA_SOURCE_ROOT CSR_TEST_ROOT)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

function(read_required OUTPUT FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 4 inventory file: ${FILE_PATH}")
  endif()
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(require_count CONTENT_VARIABLE REGEX EXPECTED DESCRIPTION)
  string(REGEX MATCHALL "${REGEX}" MATCHES "${${CONTENT_VARIABLE}}")
  list(LENGTH MATCHES ACTUAL)
  if(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR "${DESCRIPTION} count mismatch: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

function(require_absent CONTENT_VARIABLE TOKEN DESCRIPTION)
  string(FIND "${${CONTENT_VARIABLE}}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR "${DESCRIPTION} contains prohibited token: ${TOKEN}")
  endif()
endfunction()
function(require_step4_bridge_count CONTENT_VARIABLE REGEX EXPECTED DESCRIPTION)
  string(REGEX MATCHALL "${REGEX}" MATCHES "${${CONTENT_VARIABLE}}")
  list(LENGTH MATCHES ACTUAL)
  if(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 4 bridge detector rejected ${DESCRIPTION}: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

function(check_step4_session_bridge PUBLIC_FILE INTERNAL_FILE SOURCE_FILE)
  read_required(BRIDGE_PUBLIC_CONTENT "${PUBLIC_FILE}")
  read_required(BRIDGE_INTERNAL_CONTENT "${INTERNAL_FILE}")
  read_required(BRIDGE_SOURCE_CONTENT "${SOURCE_FILE}")
  foreach(HEADER_CONTENT_VARIABLE BRIDGE_PUBLIC_CONTENT BRIDGE_INTERNAL_CONTENT)
    require_step4_bridge_count(
        ${HEADER_CONTENT_VARIABLE} "exportCsrReferenceInputCandidate\\(" 0
        "exporter call outside bridge source")
    require_step4_bridge_count(
        ${HEADER_CONTENT_VARIABLE} "replayFifoLevelization\\(" 0
        "FIFO call outside bridge source")
    require_step4_bridge_count(
        ${HEADER_CONTENT_VARIABLE} "validateReferenceInputForSession\\(" 0
        "session validator call in bridge header")
  endforeach()
  require_step4_bridge_count(
      BRIDGE_SOURCE_CONTENT "exportCsrReferenceInputCandidate\\(" 1
      "bridge exporter call count")
  require_step4_bridge_count(
      BRIDGE_SOURCE_CONTENT "replayFifoLevelization\\(" 1
      "bridge FIFO call count")
  require_step4_bridge_count(
      BRIDGE_SOURCE_CONTENT "validateReferenceInputForSession\\(" 0
      "bridge direct session-validator call count")
endfunction()

if(DEFINED CSR_STEP4_BRIDGE_DETECTOR_MODE)
  foreach(REQUIRED_DETECTOR_VARIABLE
          CSR_STEP4_BRIDGE_PUBLIC CSR_STEP4_BRIDGE_INTERNAL CSR_STEP4_BRIDGE_SOURCE)
    if(NOT DEFINED ${REQUIRED_DETECTOR_VARIABLE})
      message(FATAL_ERROR "${REQUIRED_DETECTOR_VARIABLE} is required")
    endif()
  endforeach()
  check_step4_session_bridge("${CSR_STEP4_BRIDGE_PUBLIC}"
                             "${CSR_STEP4_BRIDGE_INTERNAL}"
                             "${CSR_STEP4_BRIDGE_SOURCE}")
  return()
endif()

function(run_step4_bridge_detector NAME PUBLIC_FILE INTERNAL_FILE SOURCE_FILE EXPECT_SUCCESS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              -DISTA_SOURCE_ROOT=unused -DCSR_TEST_ROOT=unused
              -DCSR_STEP4_BRIDGE_DETECTOR_MODE=TRUE
              "-DCSR_STEP4_BRIDGE_PUBLIC=${PUBLIC_FILE}"
              "-DCSR_STEP4_BRIDGE_INTERNAL=${INTERNAL_FILE}"
              "-DCSR_STEP4_BRIDGE_SOURCE=${SOURCE_FILE}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 4 bridge safe self-test ${NAME} failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 4 bridge detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 4 bridge poison self-test ${NAME} failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

if(DEFINED CSR_BUILD_DIR)
  set(STEP4_DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step4-bridge-detector")
  set(STEP4_DETECTOR_PUBLIC "${STEP4_DETECTOR_DIR}/CsrSessionSnapshotAdapter.hpp")
  set(STEP4_DETECTOR_INTERNAL "${STEP4_DETECTOR_DIR}/CsrSessionSnapshotAdapterInternal.hpp")
  set(STEP4_DETECTOR_SOURCE "${STEP4_DETECTOR_DIR}/CsrSessionSnapshotAdapter.cpp")
  file(REMOVE_RECURSE "${STEP4_DETECTOR_DIR}")
  file(MAKE_DIRECTORY "${STEP4_DETECTOR_DIR}")
  file(WRITE "${STEP4_DETECTOR_PUBLIC}" "struct PublicSafe {};\n")
  file(WRITE "${STEP4_DETECTOR_INTERNAL}" "struct InternalSafe {};\n")
  file(WRITE "${STEP4_DETECTOR_SOURCE}"
       "void bridge() { exportCsrReferenceInputCandidate(); replayFifoLevelization(); }\n")
  run_step4_bridge_detector(safe "${STEP4_DETECTOR_PUBLIC}" "${STEP4_DETECTOR_INTERNAL}"
                            "${STEP4_DETECTOR_SOURCE}" TRUE)
  file(APPEND "${STEP4_DETECTOR_PUBLIC}" "void wrong() { replayFifoLevelization(); }\n")
  run_step4_bridge_detector(fifo-in-public "${STEP4_DETECTOR_PUBLIC}" "${STEP4_DETECTOR_INTERNAL}"
                            "${STEP4_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP4_DETECTOR_PUBLIC}" "struct PublicSafe {};\n")
  file(APPEND "${STEP4_DETECTOR_INTERNAL}"
       "void wrong() { exportCsrReferenceInputCandidate(); }\n")
  run_step4_bridge_detector(exporter-in-internal "${STEP4_DETECTOR_PUBLIC}" "${STEP4_DETECTOR_INTERNAL}"
                            "${STEP4_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP4_DETECTOR_INTERNAL}" "struct InternalSafe {};\n")
  file(WRITE "${STEP4_DETECTOR_SOURCE}" "void bridge() { exportCsrReferenceInputCandidate(); }\n")
  run_step4_bridge_detector(missing-sixth-fifo "${STEP4_DETECTOR_PUBLIC}" "${STEP4_DETECTOR_INTERNAL}"
                            "${STEP4_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP4_DETECTOR_SOURCE}"
       "void bridge() { exportCsrReferenceInputCandidate(); exportCsrReferenceInputCandidate(); replayFifoLevelization(); }\n")
  run_step4_bridge_detector(extra-exporter "${STEP4_DETECTOR_PUBLIC}" "${STEP4_DETECTOR_INTERNAL}"
                            "${STEP4_DETECTOR_SOURCE}" FALSE)
  file(WRITE "${STEP4_DETECTOR_SOURCE}"
       "void bridge() { exportCsrReferenceInputCandidate(); replayFifoLevelization(); validateReferenceInputForSession(); }\n")
  run_step4_bridge_detector(direct-validator "${STEP4_DETECTOR_PUBLIC}" "${STEP4_DETECTOR_INTERNAL}"
                            "${STEP4_DETECTOR_SOURCE}" FALSE)
  file(REMOVE_RECURSE "${STEP4_DETECTOR_DIR}")
  if(EXISTS "${STEP4_DETECTOR_DIR}")
    message(FATAL_ERROR "Step 4 bridge detector cleanup failed")
  endif()
endif()


set(ADAPTER_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr")
set(CORE_DIR "${ISTA_SOURCE_ROOT}/source/module/timing_csr")
set(SNAPSHOT_DIR "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot")
set(ADAPTER_SOURCE "${ADAPTER_DIR}/CsrReferenceInputAdapter.cpp")
set(ADAPTER_HEADER "${ADAPTER_DIR}/TimingGraphCsrAdapter.hpp")
set(SCATTER_SOURCE "${ADAPTER_DIR}/TimingResultScatter.cpp")
set(SCATTER_VALIDATION_SOURCE "${ADAPTER_DIR}/TimingResultScatterValidation.cpp")
set(INPUT_VALIDATOR "${CORE_DIR}/CsrReferenceInputValidator.cpp")
set(NATIVE_VALIDATOR "${CORE_DIR}/CsrReferenceNativeModelValidator.cpp")
set(PAYLOAD_SOURCE "${ADAPTER_DIR}/CsrReferenceFingerprintPayload.cpp")
set(PAYLOAD_HEADER "${ADAPTER_DIR}/CsrReferenceFingerprintPayload.hpp")
set(FINGERPRINT_SOURCE "${ADAPTER_DIR}/CsrFingerprint.cpp")
set(FINGERPRINT_HEADER "${ADAPTER_DIR}/CsrFingerprint.hpp")
set(GRAPH_SOURCE "${ADAPTER_DIR}/TimingGraphCsrAdapter.cpp")
set(TIMING_EDIT_RESOLVER "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/timing_edit/TimingEditResolver.cpp")
set(TIMING_EDIT_PREPARER "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/timing_edit/NativeTimingEditPreparer.cpp")
set(NATIVE_INTERNAL "${CORE_DIR}/CsrReferenceNativeModelValidatorInternal.hpp")
set(REFERENCE_HEADER "${CORE_DIR}/CsrReferenceValidator.hpp")
set(FIFO_SOURCE "${CORE_DIR}/CsrFifoLevelization.cpp")
set(FIFO_HEADER "${CORE_DIR}/CsrFifoLevelization.hpp")
set(SESSION_BRIDGE_PUBLIC "${ADAPTER_DIR}/CsrSessionSnapshotAdapter.hpp")
set(SESSION_BRIDGE_INTERNAL "${ADAPTER_DIR}/CsrSessionSnapshotAdapterInternal.hpp")
set(SESSION_BRIDGE_SOURCE "${ADAPTER_DIR}/CsrSessionSnapshotAdapter.cpp")

check_step4_session_bridge("${SESSION_BRIDGE_PUBLIC}" "${SESSION_BRIDGE_INTERNAL}"
                           "${SESSION_BRIDGE_SOURCE}")

read_required(ADAPTER_CONTENT "${ADAPTER_SOURCE}")
require_count(ADAPTER_CONTENT "exportCsrReferenceInputCandidate\\(" 1 "reference exporter definition")
require_count(ADAPTER_CONTENT "exportCsrGraphSnapshot\\(" 1 "reference exporter graph capture")
require_count(ADAPTER_CONTENT "validateAnalysisContextIntrinsic\\(" 1 "reference exporter pre-Database context validation")
require_count(ADAPTER_CONTENT "validateParasiticContextCrossField\\(" 1 "reference exporter post-projection context validation")
require_count(ADAPTER_CONTENT "makeReferenceInputIdentity\\(" 1 "reference exporter identity construction")
require_count(ADAPTER_CONTENT "validateReferenceInputForSession\\(" 1 "reference exporter final session validation")
require_absent(ADAPTER_CONTENT "replayFifoLevelization(" "reference exporter")

read_required(INPUT_CONTENT "${INPUT_VALIDATOR}")
require_count(INPUT_CONTENT "validateReferenceInputIntrinsic\\(" 2 "input intrinsic definition/call")
require_count(INPUT_CONTENT "validateReferenceInput\\(" 2 "input self definition/session call")
require_count(INPUT_CONTENT "validateReferenceInputForSession\\(" 1 "session validator definition")
require_count(INPUT_CONTENT "validateAnalysisContextIntrinsic\\(" 1 "input intrinsic context call")
require_count(INPUT_CONTENT "validateParasiticContextCrossField\\(" 1 "input intrinsic parasitic cross-field call")
require_count(INPUT_CONTENT "makeReferenceInputIdentity\\(" 1 "input self identity call")
require_absent(INPUT_CONTENT "makeSnapshotIdentity(" "reference input validators")
require_absent(INPUT_CONTENT "buildReferenceFingerprintPayloads(" "reference input validators")

read_required(NATIVE_CONTENT "${NATIVE_VALIDATOR}")
require_count(NATIVE_CONTENT "validateAnalysisContextIntrinsic\\(" 1 "analysis-context intrinsic definition")
require_count(NATIVE_CONTENT "validateParasiticContextCrossField\\(" 1 "parasitic cross-field definition")

read_required(PAYLOAD_CONTENT "${PAYLOAD_SOURCE}")
require_count(PAYLOAD_CONTENT "buildReferenceFingerprintPayloads\\(" 2 "payload builder definition/identity call")
require_count(PAYLOAD_CONTENT "makeReferenceInputIdentity\\(" 1 "reference identity helper definition")
require_count(PAYLOAD_CONTENT "makeSnapshotIdentity\\(" 1 "reference identity snapshot helper call")

set(STEP4_ADAPTER_FILES
    "${ADAPTER_DIR}/CsrReferenceInputAdapter.cpp"
    "${ADAPTER_DIR}/CsrConstraintExport.cpp"
    "${ADAPTER_DIR}/CsrClockSeedExport.cpp"
    "${ADAPTER_DIR}/CsrTimingCheckExport.cpp"
    "${ADAPTER_DIR}/CsrLibertyExport.cpp"
    "${ADAPTER_DIR}/CsrLoadExport.cpp"
    "${ADAPTER_DIR}/CsrParasiticExport.cpp"
)
foreach(STEP4_ADAPTER_FILE IN LISTS STEP4_ADAPTER_FILES)
  read_required(STEP4_ADAPTER_CONTENT "${STEP4_ADAPTER_FILE}")
  require_absent(STEP4_ADAPTER_CONTENT "replayFifoLevelization(" "Step 4 adapter source ${STEP4_ADAPTER_FILE}")
endforeach()

file(GLOB_RECURSE PRODUCTION_FILES LIST_DIRECTORIES FALSE
    "${ISTA_SOURCE_ROOT}/source/*.cpp"
    "${ISTA_SOURCE_ROOT}/source/*.hpp"
)
set(EXACT_CALLSITE_KEYS
    ANALYSIS_CONTEXT
    PARASITIC_CROSS_FIELD
    INPUT_INTRINSIC
    INPUT_SESSION
    PAYLOAD_BUILDER
    REFERENCE_IDENTITY
    SNAPSHOT_IDENTITY
    GRAPH_EXPORT
    FIFO_REPLAY
)
set(CALLSITE_REGEX_ANALYSIS_CONTEXT "validateAnalysisContextIntrinsic\\(")
set(CALLSITE_COUNT_ANALYSIS_CONTEXT 4)
set(CALLSITE_FILES_ANALYSIS_CONTEXT
    "${REFERENCE_HEADER}" "${NATIVE_VALIDATOR}" "${INPUT_VALIDATOR}" "${ADAPTER_SOURCE}")
set(CALLSITE_REGEX_PARASITIC_CROSS_FIELD "validateParasiticContextCrossField\\(")
set(CALLSITE_COUNT_PARASITIC_CROSS_FIELD 4)
set(CALLSITE_FILES_PARASITIC_CROSS_FIELD
    "${NATIVE_INTERNAL}" "${NATIVE_VALIDATOR}" "${INPUT_VALIDATOR}" "${ADAPTER_SOURCE}")
set(CALLSITE_REGEX_INPUT_INTRINSIC "validateReferenceInputIntrinsic\\(")
set(CALLSITE_COUNT_INPUT_INTRINSIC 3)
set(CALLSITE_FILES_INPUT_INTRINSIC "${REFERENCE_HEADER}" "${INPUT_VALIDATOR}")
set(CALLSITE_REGEX_INPUT_SESSION "validateReferenceInputForSession\\(")
set(CALLSITE_COUNT_INPUT_SESSION 4)
set(CALLSITE_FILES_INPUT_SESSION "${REFERENCE_HEADER}" "${INPUT_VALIDATOR}" "${ADAPTER_SOURCE}" "${SCATTER_VALIDATION_SOURCE}")
set(CALLSITE_REGEX_PAYLOAD_BUILDER "buildReferenceFingerprintPayloads\\(")
set(CALLSITE_COUNT_PAYLOAD_BUILDER 3)
set(CALLSITE_FILES_PAYLOAD_BUILDER "${PAYLOAD_HEADER}" "${PAYLOAD_SOURCE}")
set(CALLSITE_REGEX_REFERENCE_IDENTITY "makeReferenceInputIdentity\\(")
set(CALLSITE_COUNT_REFERENCE_IDENTITY 4)
set(CALLSITE_FILES_REFERENCE_IDENTITY
    "${PAYLOAD_HEADER}" "${PAYLOAD_SOURCE}" "${INPUT_VALIDATOR}" "${ADAPTER_SOURCE}")
set(CALLSITE_REGEX_SNAPSHOT_IDENTITY "makeSnapshotIdentity\\(")
set(CALLSITE_COUNT_SNAPSHOT_IDENTITY 4)
set(CALLSITE_FILES_SNAPSHOT_IDENTITY
    "${FINGERPRINT_HEADER}" "${FINGERPRINT_SOURCE}" "${PAYLOAD_SOURCE}")
set(CALLSITE_REGEX_GRAPH_EXPORT "exportCsrGraphSnapshot\\(")
set(CALLSITE_COUNT_GRAPH_EXPORT 4)
set(CALLSITE_FILES_GRAPH_EXPORT
    "${ADAPTER_HEADER}" "${GRAPH_SOURCE}" "${ADAPTER_SOURCE}" "${TIMING_EDIT_RESOLVER}")
set(CALLSITE_REGEX_FIFO_REPLAY "replayFifoLevelization\\(")
set(CALLSITE_COUNT_FIFO_REPLAY 6)
set(CALLSITE_FILES_FIFO_REPLAY
    "${FIFO_HEADER}"
    "${FIFO_SOURCE}"
    "${CORE_DIR}/CsrReferenceValidator.cpp"
    "${CORE_DIR}/CsrTimingResultValidator.cpp"
    "${SNAPSHOT_DIR}/CsrSnapshotObservationValidator.cpp"
    "${SESSION_BRIDGE_SOURCE}")
foreach(CALLSITE_KEY IN LISTS EXACT_CALLSITE_KEYS)
  set(CALLSITE_ACTUAL_COUNT_${CALLSITE_KEY} 0)
  set(CALLSITE_ACTUAL_FILES_${CALLSITE_KEY})
endforeach()
set(EXPORT_OCCURRENCE_FILES)
set(EXPORT_OCCURRENCES 0)
set(REFERENCE_VALIDATION_FILES)
set(REFERENCE_VALIDATION_OCCURRENCES 0)
set(RAW_DTO_OCCURRENCES 0)
foreach(PRODUCTION_FILE IN LISTS PRODUCTION_FILES)
  read_required(PRODUCTION_CONTENT "${PRODUCTION_FILE}")
  string(REGEX MATCHALL "exportCsrReferenceInputCandidate\\(" EXPORT_MATCHES "${PRODUCTION_CONTENT}")
  list(LENGTH EXPORT_MATCHES EXPORT_COUNT)
  if(EXPORT_COUNT GREATER 0)
    list(APPEND EXPORT_OCCURRENCE_FILES "${PRODUCTION_FILE}")
    math(EXPR EXPORT_OCCURRENCES "${EXPORT_OCCURRENCES} + ${EXPORT_COUNT}")
  endif()
  string(REGEX MATCHALL "validateReferenceInput\\(" VALIDATION_MATCHES "${PRODUCTION_CONTENT}")
  list(LENGTH VALIDATION_MATCHES VALIDATION_COUNT)
  if(VALIDATION_COUNT GREATER 0 AND PRODUCTION_FILE MATCHES "\\.cpp$")
    list(APPEND REFERENCE_VALIDATION_FILES "${PRODUCTION_FILE}")
    math(EXPR REFERENCE_VALIDATION_OCCURRENCES "${REFERENCE_VALIDATION_OCCURRENCES} + ${VALIDATION_COUNT}")
  endif()
  string(REGEX MATCHALL "CsrReferenceInputRawProjection" RAW_MATCHES "${PRODUCTION_CONTENT}")
  list(LENGTH RAW_MATCHES RAW_COUNT)
  math(EXPR RAW_DTO_OCCURRENCES "${RAW_DTO_OCCURRENCES} + ${RAW_COUNT}")
  foreach(CALLSITE_KEY IN LISTS EXACT_CALLSITE_KEYS)
    string(REGEX MATCHALL "${CALLSITE_REGEX_${CALLSITE_KEY}}" CALLSITE_MATCHES
           "${PRODUCTION_CONTENT}")
    list(LENGTH CALLSITE_MATCHES CALLSITE_MATCH_COUNT)
    if(CALLSITE_MATCH_COUNT GREATER 0)
      list(APPEND CALLSITE_ACTUAL_FILES_${CALLSITE_KEY} "${PRODUCTION_FILE}")
      set(CALLSITE_COUNT_VARIABLE "CALLSITE_ACTUAL_COUNT_${CALLSITE_KEY}")
      set(CALLSITE_CURRENT_COUNT "${${CALLSITE_COUNT_VARIABLE}}")
      math(EXPR CALLSITE_CURRENT_COUNT "${CALLSITE_CURRENT_COUNT} + ${CALLSITE_MATCH_COUNT}")
      set("${CALLSITE_COUNT_VARIABLE}" "${CALLSITE_CURRENT_COUNT}")
    endif()
  endforeach()
endforeach()

foreach(CALLSITE_KEY IN LISTS EXACT_CALLSITE_KEYS)
  list(SORT CALLSITE_ACTUAL_FILES_${CALLSITE_KEY})
  list(SORT CALLSITE_FILES_${CALLSITE_KEY})
  if(NOT "${CALLSITE_ACTUAL_COUNT_${CALLSITE_KEY}}" EQUAL "${CALLSITE_COUNT_${CALLSITE_KEY}}"
     OR NOT "${CALLSITE_ACTUAL_FILES_${CALLSITE_KEY}}" STREQUAL "${CALLSITE_FILES_${CALLSITE_KEY}}")
    message(FATAL_ERROR
            "${CALLSITE_KEY} production callsite inventory differs: count=${CALLSITE_ACTUAL_COUNT_${CALLSITE_KEY}}, files=[${CALLSITE_ACTUAL_FILES_${CALLSITE_KEY}}]")
  endif()
endforeach()

set(EXPECTED_EXPORT_FILES "${ADAPTER_SOURCE}" "${ADAPTER_HEADER}" "${SCATTER_SOURCE}"
                          "${SESSION_BRIDGE_SOURCE}" "${TIMING_EDIT_PREPARER}")
list(SORT EXPORT_OCCURRENCE_FILES)
list(SORT EXPECTED_EXPORT_FILES)
if(NOT EXPORT_OCCURRENCE_FILES STREQUAL EXPECTED_EXPORT_FILES OR NOT EXPORT_OCCURRENCES EQUAL 5)
  message(FATAL_ERROR "reference exporter inventory must include declared production consumers")
endif()

set(EXPECTED_VALIDATION_FILES
    "${CORE_DIR}/CsrReferenceInputValidator.cpp"
    "${CORE_DIR}/CsrReferenceValidator.cpp"
    "${CORE_DIR}/CsrTimingResultValidator.cpp"
    "${CORE_DIR}/CsrCpuReferenceBackend.cpp"
    "${SNAPSHOT_DIR}/CsrSnapshotObservationValidator.cpp"
)
list(SORT REFERENCE_VALIDATION_FILES)
list(SORT EXPECTED_VALIDATION_FILES)
if(NOT REFERENCE_VALIDATION_FILES STREQUAL EXPECTED_VALIDATION_FILES OR NOT REFERENCE_VALIDATION_OCCURRENCES EQUAL 6)
  message(FATAL_ERROR "validateReferenceInput production consumer inventory differs")
endif()
if(NOT RAW_DTO_OCCURRENCES EQUAL 0)
  message(FATAL_ERROR "test-only raw projection DTO leaked into production source")
endif()

read_required(PAYLOAD_HEADER_CONTENT "${PAYLOAD_HEADER}")
require_count(PAYLOAD_HEADER_CONTENT "buildReferenceFingerprintPayloads\\(" 1 "payload builder declaration")
require_count(PAYLOAD_HEADER_CONTENT "makeReferenceInputIdentity\\(" 1 "reference identity declaration")

message(STATUS "CSR Step 4 production call-site inventory passed")
