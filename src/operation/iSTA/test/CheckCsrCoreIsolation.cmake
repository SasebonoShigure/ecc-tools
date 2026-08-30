# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)
cmake_policy(SET CMP0130 NEW)

foreach(REQUIRED_VARIABLE
        CSR_CORE_ACTUAL_SOURCES_PACKED
        CSR_CORE_EXPECTED_SOURCES_PACKED
        CSR_CORE_FILES_PACKED
        CSR_CORE_OBJECTS_PACKED
        CSR_CORE_OBJECT_DIR
        CSR_BUILD_DIR
        NINJA_EXECUTABLE
        ISTA_SOURCE_ROOT
        CSR_GENERATOR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

string(REPLACE "|" ";" CSR_CORE_ACTUAL_SOURCES "${CSR_CORE_ACTUAL_SOURCES_PACKED}")
string(REPLACE "|" ";" CSR_CORE_EXPECTED_SOURCES "${CSR_CORE_EXPECTED_SOURCES_PACKED}")
string(REPLACE "|" ";" CSR_CORE_FILES "${CSR_CORE_FILES_PACKED}")
string(REPLACE "|" ";" CSR_CORE_OBJECTS "${CSR_CORE_OBJECTS_PACKED}")

function(assert_unique LIST_VARIABLE DESCRIPTION)
  set(SEEN)
  foreach(ITEM IN LISTS ${LIST_VARIABLE})
    list(FIND SEEN "${ITEM}" SEEN_INDEX)
    if(NOT SEEN_INDEX EQUAL -1)
      message(FATAL_ERROR "duplicate ${DESCRIPTION}: ${ITEM}")
    endif()
    list(APPEND SEEN "${ITEM}")
  endforeach()
endfunction()

function(assert_same_inventory ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  assert_unique(${ACTUAL_VARIABLE} "actual ${DESCRIPTION}")
  assert_unique(${EXPECTED_VARIABLE} "expected ${DESCRIPTION}")
  set(ACTUAL_SORTED ${${ACTUAL_VARIABLE}})
  set(EXPECTED_SORTED ${${EXPECTED_VARIABLE}})
  list(SORT ACTUAL_SORTED)
  list(SORT EXPECTED_SORTED)
  if(NOT "${ACTUAL_SORTED}" STREQUAL "${EXPECTED_SORTED}")
    message(FATAL_ERROR "${DESCRIPTION} mismatch; actual=[${ACTUAL_SORTED}] expected=[${EXPECTED_SORTED}]")
  endif()
endfunction()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "CSR compiler-dependency isolation requires a Ninja generator")
endif()
if(NOT EXISTS "${NINJA_EXECUTABLE}")
  message(FATAL_ERROR "Ninja executable is missing: ${NINJA_EXECUTABLE}")
endif()

assert_same_inventory(
    CSR_CORE_ACTUAL_SOURCES CSR_CORE_EXPECTED_SOURCES "CSR core source"
)

execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
    RESULT_VARIABLE TARGETS_RESULT
    OUTPUT_VARIABLE TARGETS_OUTPUT
    ERROR_VARIABLE TARGETS_ERROR
)
if(NOT TARGETS_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot read Ninja target inventory: ${TARGETS_ERROR}")
endif()
set(CSR_CORE_ACTUAL_OBJECTS)
string(REPLACE "\n" ";" TARGET_LINES "${TARGETS_OUTPUT}")
foreach(TARGET_LINE IN LISTS TARGET_LINES)
  if(TARGET_LINE MATCHES "^([^:]+\\.cpp\\.o):")
    set(CANDIDATE "${CMAKE_MATCH_1}")
    string(FIND "${CANDIDATE}" "${CSR_CORE_OBJECT_DIR}/" OBJECT_PREFIX)
    if(OBJECT_PREFIX EQUAL 0)
      list(APPEND CSR_CORE_ACTUAL_OBJECTS "${CANDIDATE}")
    endif()
  endif()
endforeach()
assert_same_inventory(
    CSR_CORE_ACTUAL_OBJECTS CSR_CORE_OBJECTS "CSR core object"
)

list(LENGTH CSR_CORE_EXPECTED_SOURCES CORE_SOURCE_COUNT)
list(LENGTH CSR_CORE_OBJECTS CORE_OBJECT_COUNT)
if(NOT CORE_SOURCE_COUNT EQUAL 18 OR NOT CORE_OBJECT_COUNT EQUAL 18)
  message(FATAL_ERROR "CSR core inventory must contain exactly 18 sources and objects")
endif()

set(PROHIBITED_TOKENS
    "CsrNativeOracle"
    "CsrCaptureMetadata"
    "CsrNativeGraphOracle"
    "CsrNativeTimingEvaluator"
    "CsrNativeTimingOracle"
    "CsrNativeSemanticKeyTestSupport"
    "NativeTimingEvaluator"
    "ClockPropagator"
    "TimingPropagator"
    "TimingAnalyzer"
    "/scatter/"
    "timing_scatter"
    "CsrGraphEncoding"
    "CsrGraphTypes"
    "CsrGraphValidator"
    "NativeGraphProjection"
    "TimingGraphCsrAdapter"
    "TimingResultScatter"
    "CsrSnapshotObservation"
    "CsrSnapshotStorage"
    "CsrSnapshotObservationTransition"
    "timing_csr_snapshot"
    "data_manager/advance"
    "Database.hpp"
    "Arc.hpp"
    "TimingPoint.hpp"
    "TimingArc.hpp"
    "TimingTable.hpp"
    "DelayCalculator"
    "STAInterface"
)

foreach(CORE_FILE IN LISTS CSR_CORE_FILES)
  if(NOT EXISTS "${CORE_FILE}")
    message(FATAL_ERROR "missing CSR core file: ${CORE_FILE}")
  endif()
  file(READ "${CORE_FILE}" CONTENT)
  foreach(PROHIBITED_TOKEN IN LISTS PROHIBITED_TOKENS)
    string(FIND "${CONTENT}" "${PROHIBITED_TOKEN}" POSITION)
    if(NOT POSITION EQUAL -1)
      message(FATAL_ERROR "CSR core isolation violation in ${CORE_FILE}: ${PROHIBITED_TOKEN}")
    endif()
  endforeach()
endforeach()

foreach(CORE_OBJECT IN LISTS CSR_CORE_OBJECTS)
  execute_process(
      COMMAND
      "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${CORE_OBJECT}"
      RESULT_VARIABLE DEPS_RESULT
      OUTPUT_VARIABLE DEPS_OUTPUT
      ERROR_VARIABLE DEPS_ERROR
  )
  if(NOT DEPS_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read compiler dependencies for ${CORE_OBJECT}: ${DEPS_ERROR}")
  endif()
  string(FIND "${DEPS_OUTPUT}" "(VALID)" VALID_POSITION)
  if(VALID_POSITION EQUAL -1)
    message(FATAL_ERROR "compiler dependencies are missing or stale for ${CORE_OBJECT}")
  endif()
  string(REPLACE "\n" ";" DEPENDENCY_LINES "${DEPS_OUTPUT}")
  foreach(DEPENDENCY_LINE IN LISTS DEPENDENCY_LINES)
    string(STRIP "${DEPENDENCY_LINE}" DEPENDENCY)
    string(FIND "${DEPENDENCY}" "${ISTA_SOURCE_ROOT}/" SOURCE_PREFIX)
    if(SOURCE_PREFIX EQUAL 0)
      foreach(TEST_ONLY_HEADER IN ITEMS
              CsrNativeTimingOracle.hpp CsrNativeSemanticKeyTestSupport.hpp)
        string(FIND "${DEPENDENCY}" "${TEST_ONLY_HEADER}" HEADER_POSITION)
        if(NOT HEADER_POSITION EQUAL -1)
          message(FATAL_ERROR
                  "test-only CSR header leaked into core object ${CORE_OBJECT}: ${DEPENDENCY}")
        endif()
      endforeach()
      list(FIND CSR_CORE_FILES "${DEPENDENCY}" ALLOWED_INDEX)
      if(ALLOWED_INDEX EQUAL -1)
        message(
            FATAL_ERROR
            "CSR core compiler dependency escapes the allow-list in ${CORE_OBJECT}: ${DEPENDENCY}"
        )
      endif()
    endif()
  endforeach()
endforeach()
function(require_literal_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
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
            "${DESCRIPTION} count mismatch: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

set(TYPED_PUBLIC "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrReferenceValidator.hpp")
set(TYPED_IMPLEMENTATION "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrReferenceValidator.cpp")
set(TYPED_DETAIL "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrReferenceValidatorDetail.hpp")
set(TYPED_INTERNAL "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrTimingResultValidatorInternal.hpp")
set(TYPED_RESULT "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrTimingResultValidator.cpp")
set(TYPED_ARC "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrTimingResultArcValidator.cpp")
set(TYPED_IDENTITY "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrSnapshotIdentityValidator.cpp")
set(TYPED_MODIFIED_FILES
    "${TYPED_PUBLIC}" "${TYPED_IMPLEMENTATION}" "${TYPED_DETAIL}" "${TYPED_IDENTITY}"
    "${TYPED_RESULT}" "${TYPED_ARC}" "${TYPED_INTERNAL}")
list(LENGTH TYPED_MODIFIED_FILES TYPED_MODIFIED_COUNT)
if(NOT TYPED_MODIFIED_COUNT EQUAL 7)
  message(FATAL_ERROR "typed-validator modified-file inventory must contain exactly seven files")
endif()
set(TYPED_SCATTER "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/TimingResultScatterValidation.cpp")
foreach(TYPED_FILE IN LISTS TYPED_MODIFIED_FILES ITEMS "${TYPED_SCATTER}")
  if(NOT EXISTS "${TYPED_FILE}")
    message(FATAL_ERROR "missing typed-validator file: ${TYPED_FILE}")
  endif()
endforeach()
set(SNAPSHOT_POLICY "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotInvalidationPolicyInternal.hpp")
set(SCATTER_ORCHESTRATOR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/TimingResultScatter.cpp")
set(EXPECTED_INVALIDATION_FILES "${TYPED_PUBLIC}" "${TYPED_IMPLEMENTATION}" "${SNAPSHOT_POLICY}" "${SCATTER_ORCHESTRATOR}")
set(EXPECTED_CLASSIFIED_FILES "${TYPED_INTERNAL}" "${TYPED_RESULT}" "${TYPED_SCATTER}")
file(GLOB_RECURSE PRODUCTION_VALIDATOR_FILES LIST_DIRECTORIES FALSE
     "${ISTA_SOURCE_ROOT}/source/*.cpp" "${ISTA_SOURCE_ROOT}/source/*.hpp")
set(ACTUAL_INVALIDATION_FILES)
set(ACTUAL_CLASSIFIED_FILES)
foreach(PRODUCTION_FILE IN LISTS PRODUCTION_VALIDATOR_FILES)
  file(STRINGS "${PRODUCTION_FILE}" INVALIDATION_LINES
       REGEX "firstCsrInvalidationReason\\(")
  if(INVALIDATION_LINES)
    list(APPEND ACTUAL_INVALIDATION_FILES "${PRODUCTION_FILE}")
  endif()
  file(STRINGS "${PRODUCTION_FILE}" CLASSIFIED_LINES
       REGEX "validateTimingResultClassified\\(")
  if(CLASSIFIED_LINES)
    list(APPEND ACTUAL_CLASSIFIED_FILES "${PRODUCTION_FILE}")
  endif()
endforeach()
assert_same_inventory(ACTUAL_INVALIDATION_FILES EXPECTED_INVALIDATION_FILES
                      "invalidation helper declaration/definition/callers")
assert_same_inventory(ACTUAL_CLASSIFIED_FILES EXPECTED_CLASSIFIED_FILES
                      "classified validator declaration/definition/callers")
file(READ "${TYPED_PUBLIC}" PUBLIC_CONTENT)
file(READ "${TYPED_IMPLEMENTATION}" IMPLEMENTATION_CONTENT)
file(READ "${SNAPSHOT_POLICY}" SNAPSHOT_POLICY_CONTENT)
file(READ "${SCATTER_ORCHESTRATOR}" SCATTER_ORCHESTRATOR_CONTENT)
require_literal_count(PUBLIC_CONTENT "firstCsrInvalidationReason(" 1 "invalidation declaration")
require_literal_count(IMPLEMENTATION_CONTENT "firstCsrInvalidationReason(" 1 "invalidation definition")
require_literal_count(SNAPSHOT_POLICY_CONTENT "firstCsrInvalidationReason(" 1 "snapshot invalidation delegate")
require_literal_count(SCATTER_ORCHESTRATOR_CONTENT "firstCsrInvalidationReason(" 2 "scatter invalidation callers")
file(READ "${TYPED_DETAIL}" DETAIL_CONTENT)
string(REGEX MATCH "enum class CsrTimingResultViolationClass[^}]+}" CLASS_ENUM
       "${DETAIL_CONTENT}")
foreach(CLASS kSchema kSession kGeneration kSourceIdentity
              kShape kNumeric kIdMapping kPredecessor)
  require_literal_count(CLASS_ENUM "${CLASS}" 1 "typed result class ${CLASS}")
endforeach()

file(READ "${TYPED_SCATTER}" SCATTER_CONTENT)
foreach(MAPPING
        "kSchema|CsrScatterCode::kSchemaMismatch"
        "kSession|CsrScatterCode::kSessionMismatch"
        "kGeneration|CsrScatterCode::kGenerationMismatch"
        "kSourceIdentity|CsrScatterCode::kSourceIdentityMismatch"
        "kShape|CsrScatterCode::kShapeMismatch"
        "kNumeric|CsrScatterCode::kResultNumericInvalid"
        "kIdMapping|CsrScatterCode::kIdMappingMismatch"
        "kPredecessor|CsrScatterCode::kPredecessorMismatch")
  string(REPLACE "|" ";" PARTS "${MAPPING}")
  list(GET PARTS 0 CLASS)
  list(GET PARTS 1 CODE)
  require_literal_count(
      SCATTER_CONTENT "case CsrTimingResultViolationClass::${CLASS}:" 1
      "typed scatter case ${CLASS}")
  require_literal_count(SCATTER_CONTENT "${CODE}" 1 "typed scatter code ${CODE}")
endforeach()
foreach(PROHIBITED_TOKEN "default:" ".field" ".message"
                         "validateTimingResult(" "appendViolations(")
  string(FIND "${SCATTER_CONTENT}" "${PROHIBITED_TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR "typed scatter uses prohibited token: ${PROHIBITED_TOKEN}")
  endif()
endforeach()

file(READ "${TYPED_INTERNAL}" INTERNAL_CONTENT)
file(READ "${TYPED_RESULT}" RESULT_CONTENT)
file(READ "${TYPED_ARC}" ARC_CONTENT)
file(READ "${TYPED_IDENTITY}" IDENTITY_CONTENT)
require_literal_count(INTERNAL_CONTENT "validateTimingResultClassified(" 1
                      "classified validator declaration")
require_literal_count(RESULT_CONTENT "validateTimingResultClassified(" 2
                      "classified validator definition/legacy caller")
require_literal_count(SCATTER_CONTENT "validateTimingResultClassified(" 1
                      "classified scatter caller")
require_literal_count(RESULT_CONTENT "validateReferenceInput(input)" 1
                      "legacy timing-result input precheck")
require_literal_count(RESULT_CONTENT "input.unsupported_reasons.empty()" 1
                      "legacy timing-result support precheck")
require_literal_count(IDENTITY_CONTENT "validateSnapshotIdentityClassified(" 3
                      "typed identity sink/legacy projection")
require_literal_count(RESULT_CONTENT "validateSnapshotIdentityClassified(" 1
                      "typed result identity caller")
foreach(RULE_FILE IN ITEMS "${TYPED_RESULT}" "${TYPED_ARC}")
  file(READ "${RULE_FILE}" RULE_CONTENT)
  string(REGEX MATCH "(^|[^A-Za-z0-9_])Violations([ \t*&])" UNTYPED
         "${RULE_CONTENT}")
  if(NOT "${UNTYPED}" STREQUAL "")
    message(FATAL_ERROR "untyped Violations leaked into result rules: ${RULE_FILE}")
  endif()
  string(FIND "${RULE_CONTENT}" "appendViolations(" APPEND_POSITION)
  if(NOT APPEND_POSITION EQUAL -1)
    message(FATAL_ERROR "untyped appendViolations leaked into result rules: ${RULE_FILE}")
  endif()
  file(STRINGS "${RULE_FILE}" REJECT_LINES REGEX "reject\\(errors")
  foreach(REJECT_LINE IN LISTS REJECT_LINES)
    string(FIND "${REJECT_LINE}" "ResultClass::" CLASS_POSITION)
    if(CLASS_POSITION EQUAL -1)
      message(FATAL_ERROR
              "result reject lacks explicit class: ${RULE_FILE}: ${REJECT_LINE}")
    endif()
  endforeach()
  foreach(HELPER validateOptionalSoa checkedProduct validateOffsets)
    string(REGEX MATCH "${HELPER}\\([^;]*errors,[ \t\r\n]*\"" UNTYPED_HELPER
           "${RULE_CONTENT}")
    if(NOT "${UNTYPED_HELPER}" STREQUAL "")
      message(FATAL_ERROR
              "result helper lacks explicit class: ${RULE_FILE}: ${HELPER}")
    endif()
  endforeach()
endforeach()

message(STATUS "CSR core source and compiler-dependency isolation passed")
