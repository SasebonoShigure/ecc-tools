# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

function(require_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 5 inventory file: ${FILE_PATH}")
  endif()
endfunction()
function(read_required OUTPUT FILE_PATH)
  require_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()
function(require_absent FILE_PATH TOKEN DESCRIPTION)
  read_required(CONTENT "${FILE_PATH}")
  string(FIND "${CONTENT}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR "${DESCRIPTION} contains prohibited token ${TOKEN}: ${FILE_PATH}")
  endif()
endfunction()
function(require_present FILE_PATH TOKEN DESCRIPTION)
  read_required(CONTENT "${FILE_PATH}")
  string(FIND "${CONTENT}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "${DESCRIPTION} is missing required token ${TOKEN}: ${FILE_PATH}")
  endif()
endfunction()

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
    message(FATAL_ERROR "${DESCRIPTION} count mismatch: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

function(require_under_700 FILE_PATH)
  require_file("${FILE_PATH}")
  file(STRINGS "${FILE_PATH}" LINES)
  list(LENGTH LINES LINE_COUNT)
  if(LINE_COUNT GREATER_EQUAL 700)
    message(STATUS "Step 5 advisory line target reached or exceeded: ${FILE_PATH}: ${LINE_COUNT}")
  endif()
endfunction()

function(assert_same_inventory ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  set(ACTUAL ${${ACTUAL_VARIABLE}})
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  list(SORT ACTUAL)
  list(SORT EXPECTED)
  if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "${DESCRIPTION} mismatch; actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()

if(DEFINED CSR_STEP5_DETECTOR_MODE)
  foreach(REQUIRED_SELFTEST_VARIABLE CSR_STEP5_DETECTOR_FILE CSR_STEP5_DETECTOR_TOKEN)
    if(NOT DEFINED ${REQUIRED_SELFTEST_VARIABLE})
      message(FATAL_ERROR "${REQUIRED_SELFTEST_VARIABLE} is required")
    endif()
  endforeach()
  if(CSR_STEP5_DETECTOR_MODE STREQUAL "symbol")
    if(NOT DEFINED NM_EXECUTABLE)
      message(FATAL_ERROR "NM_EXECUTABLE is required")
    endif()
    execute_process(
        COMMAND "${NM_EXECUTABLE}" -uC "${CSR_STEP5_DETECTOR_FILE}"
        RESULT_VARIABLE DETECTOR_RESULT
        OUTPUT_VARIABLE DETECTOR_CONTENT
        ERROR_VARIABLE DETECTOR_ERROR
    )
    if(NOT DETECTOR_RESULT EQUAL 0)
      message(FATAL_ERROR "symbol detector cannot inspect fixture: ${DETECTOR_ERROR}")
    endif()
  else()
    read_required(DETECTOR_CONTENT "${CSR_STEP5_DETECTOR_FILE}")
  endif()
  string(FIND "${DETECTOR_CONTENT}" "${CSR_STEP5_DETECTOR_TOKEN}" DETECTOR_POSITION)
  if(NOT DETECTOR_POSITION EQUAL -1)
    message(FATAL_ERROR
            "Step 5 ${CSR_STEP5_DETECTOR_MODE} detector rejected poison token: ${CSR_STEP5_DETECTOR_TOKEN}")
  endif()
  return()
endif()

foreach(REQUIRED_VARIABLE
        ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR NINJA_EXECUTABLE
        CXX_COMPILER NM_EXECUTABLE CSR_GENERATOR
        CSR_TABLE_SOURCES_PACKED CSR_CONTRACT_SOURCES_PACKED
        CSR_PROPERTY_SOURCES_PACKED CSR_PUBLIC_HEADER_SOURCES_PACKED)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

set(EXPECTED_TABLE_SOURCES
    CsrTableLookupContractTest.cpp)
set(EXPECTED_CONTRACT_SOURCES
    CsrCpuReferenceContractTest.cpp
    CsrCpuReferenceOutcomeContract.cpp
    CsrCpuReferenceArcContract.cpp
    CsrCpuReferencePropagationContract.cpp
    CsrCpuReferenceTestFixture.cpp)
set(EXPECTED_PROPERTY_SOURCES
    CsrCpuReferencePropertyTest.cpp
    CsrCpuReferenceTestFixture.cpp
    CsrCpuReferenceSlowOracleTable.cpp
    CsrCpuReferenceSlowOracleTransfer.cpp
    CsrCpuReferenceSlowOraclePropagation.cpp)
set(EXPECTED_PUBLIC_HEADER_SOURCES
    CsrCpuReferencePublicHeadersMain.cpp
    public_header/CsrTableLookupHeaderTest.cpp
    public_header/CsrCpuReferenceBackendHeaderTest.cpp
    CsrCpuReferenceHeaderIncludeOrderTest.cpp
    CsrCpuReferenceHeaderReverseIncludeOrderTest.cpp)

string(REPLACE "|" ";" ACTUAL_TABLE_SOURCES
       "${CSR_TABLE_SOURCES_PACKED}")
string(REPLACE "|" ";" ACTUAL_CONTRACT_SOURCES
       "${CSR_CONTRACT_SOURCES_PACKED}")
string(REPLACE "|" ";" ACTUAL_PROPERTY_SOURCES
       "${CSR_PROPERTY_SOURCES_PACKED}")
string(REPLACE "|" ";" ACTUAL_PUBLIC_HEADER_SOURCES
       "${CSR_PUBLIC_HEADER_SOURCES_PACKED}")
assert_same_inventory(
    ACTUAL_TABLE_SOURCES EXPECTED_TABLE_SOURCES
    "Step 5 table target source")
assert_same_inventory(
    ACTUAL_CONTRACT_SOURCES EXPECTED_CONTRACT_SOURCES
    "Step 5 contract target source")
assert_same_inventory(
    ACTUAL_PROPERTY_SOURCES EXPECTED_PROPERTY_SOURCES
    "Step 5 property target source")
assert_same_inventory(
    ACTUAL_PUBLIC_HEADER_SOURCES EXPECTED_PUBLIC_HEADER_SOURCES
    "Step 5 public-header target source")

string(JOIN "|" EXPECTED_TABLE_SOURCES_PACKED
       ${EXPECTED_TABLE_SOURCES})
string(JOIN "|" EXPECTED_CONTRACT_SOURCES_PACKED
       ${EXPECTED_CONTRACT_SOURCES})
string(JOIN "|" EXPECTED_PROPERTY_SOURCES_PACKED
       ${EXPECTED_PROPERTY_SOURCES})
string(JOIN "|" EXPECTED_PUBLIC_HEADER_SOURCES_PACKED
       ${EXPECTED_PUBLIC_HEADER_SOURCES})

function(run_detector_selftest MODE FILE_PATH TOKEN EXPECT_SUCCESS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
      "-DCSR_STEP5_DETECTOR_MODE=${MODE}"
      "-DCSR_STEP5_DETECTOR_FILE=${FILE_PATH}"
      "-DCSR_STEP5_DETECTOR_TOKEN=${TOKEN}"
      "-DNM_EXECUTABLE=${NM_EXECUTABLE}"
      -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT
      OUTPUT_VARIABLE STDOUT
      ERROR_VARIABLE STDERR
  )
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 5 ${MODE} safe-fixture self-test failed: ${STDOUT}${STDERR}")
    endif()
  else()
    string(FIND "${STDERR}" "Step 5 ${MODE} detector rejected poison token: ${TOKEN}"
           DIAGNOSTIC_POSITION)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC_POSITION EQUAL -1)
      message(FATAL_ERROR "Step 5 ${MODE} poison-fixture self-test failed: ${STDOUT}${STDERR}")
    endif()
  endif()
endfunction()

set(DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step5-detector-selftest")
file(REMOVE_RECURSE "${DETECTOR_DIR}")
file(MAKE_DIRECTORY "${DETECTOR_DIR}")
file(WRITE "${DETECTOR_DIR}/source-safe.cpp" "void sourceSafe() {}\n")
file(WRITE "${DETECTOR_DIR}/source-poison.cpp" "void CsrCaptureMetadataPoison() {}\n")
file(WRITE "${DETECTOR_DIR}/deps-safe.txt" "/usr/include/vector\n")
file(WRITE "${DETECTOR_DIR}/deps-poison.txt" "/source/CsrCaptureMetadata.hpp\n")
file(WRITE "${DETECTOR_DIR}/symbol-safe.cpp" "void symbolSafe() {}\n")
file(WRITE "${DETECTOR_DIR}/symbol-poison.cpp"
     "extern void CsrNativeOraclePoison();\nvoid symbolPoison() { CsrNativeOraclePoison(); }\n")
foreach(SYMBOL_FIXTURE IN ITEMS safe poison)
  execute_process(
      COMMAND "${CXX_COMPILER}" -std=c++20 -c
      "${DETECTOR_DIR}/symbol-${SYMBOL_FIXTURE}.cpp"
      -o "${DETECTOR_DIR}/symbol-${SYMBOL_FIXTURE}.o"
      RESULT_VARIABLE COMPILE_RESULT
      OUTPUT_VARIABLE COMPILE_STDOUT
      ERROR_VARIABLE COMPILE_STDERR
  )
  if(NOT COMPILE_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot compile symbol detector fixture: ${COMPILE_STDOUT}${COMPILE_STDERR}")
  endif()
endforeach()
run_detector_selftest(source "${DETECTOR_DIR}/source-safe.cpp" CsrCaptureMetadata TRUE)
run_detector_selftest(source "${DETECTOR_DIR}/source-poison.cpp" CsrCaptureMetadata FALSE)
run_detector_selftest(depfile "${DETECTOR_DIR}/deps-safe.txt" CsrCaptureMetadata.hpp TRUE)
run_detector_selftest(depfile "${DETECTOR_DIR}/deps-poison.txt" CsrCaptureMetadata.hpp FALSE)
run_detector_selftest(symbol "${DETECTOR_DIR}/symbol-safe.o" CsrNativeOraclePoison TRUE)
run_detector_selftest(symbol "${DETECTOR_DIR}/symbol-poison.o" CsrNativeOraclePoison FALSE)
file(REMOVE_RECURSE "${DETECTOR_DIR}")
if(EXISTS "${DETECTOR_DIR}")
  message(FATAL_ERROR "Step 5 detector self-test cleanup failed")
endif()

set(CORE_DIR "${ISTA_SOURCE_ROOT}/source/module/timing_csr")
set(EXPECTED_PRODUCTION_FILES
    "${CORE_DIR}/CsrTableLookup.hpp"
    "${CORE_DIR}/CsrTableLookup.cpp"
    "${CORE_DIR}/CsrCpuReferenceBackend.hpp"
    "${CORE_DIR}/CsrCpuReferenceBackend.cpp"
    "${CORE_DIR}/CsrCpuReferenceBackendInternal.hpp"
    "${CORE_DIR}/CsrCpuReferenceResultInitialization.cpp"
    "${CORE_DIR}/CsrCpuReferenceArcTransfer.cpp"
    "${CORE_DIR}/CsrCpuReferenceForwardPropagation.cpp"
    "${CORE_DIR}/CsrCpuReferenceRequiredPropagation.cpp"
)
file(GLOB ACTUAL_PRODUCTION_FILES LIST_DIRECTORIES FALSE
     "${CORE_DIR}/CsrTableLookup.*" "${CORE_DIR}/CsrCpuReference*")
assert_same_inventory(
    ACTUAL_PRODUCTION_FILES EXPECTED_PRODUCTION_FILES "Step 5 production file inventory"
)

set(EXPECTED_TEST_FILES
    "${CSR_TEST_ROOT}/CsrTableLookupContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceOutcomeContract.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceArcContract.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferencePropagationContract.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferencePropertyTest.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceTestFixture.hpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceTestFixture.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracle.hpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleInternal.hpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleTable.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleTransfer.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOraclePropagation.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferencePublicHeadersMain.cpp"
    "${CSR_TEST_ROOT}/public_header/CsrTableLookupHeaderTest.cpp"
    "${CSR_TEST_ROOT}/public_header/CsrCpuReferenceBackendHeaderTest.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceHeaderIncludeOrderTest.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceHeaderReverseIncludeOrderTest.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceTests.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep5SourceInventory.cmake"
)
file(GLOB ACTUAL_TEST_FILES LIST_DIRECTORIES FALSE
     "${CSR_TEST_ROOT}/CsrCpuReference*"
     "${CSR_TEST_ROOT}/CsrTableLookupContractTest.cpp"
     "${CSR_TEST_ROOT}/CheckCsrStep5SourceInventory.cmake"
     "${CSR_TEST_ROOT}/public_header/CsrTableLookupHeaderTest.cpp"
     "${CSR_TEST_ROOT}/public_header/CsrCpuReferenceBackendHeaderTest.cpp")
assert_same_inventory(ACTUAL_TEST_FILES EXPECTED_TEST_FILES "Step 5 test file inventory")
string(CONCAT FORBIDDEN_CAST_TOKEN "const" "_cast")
foreach(STEP5_FILE IN LISTS EXPECTED_PRODUCTION_FILES EXPECTED_TEST_FILES)
  require_under_700("${STEP5_FILE}")
  require_absent("${STEP5_FILE}" "${FORBIDDEN_CAST_TOKEN}" "Step 5 source")
  if(NOT STEP5_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    foreach(STEP6_PROHIBITED_TOKEN IN ITEMS CsrNativeTimingOracle TimingResultScatter CsrNativeSemanticKeyTestSupport)
      require_absent("${STEP5_FILE}" "${STEP6_PROHIBITED_TOKEN}" "Step 5 source")
    endforeach()
  endif()
endforeach()
function(assert_quote_includes FILE_PATH)
  file(STRINGS "${FILE_PATH}" INCLUDE_LINES REGEX "^#[ \t]*include[ \t]+\"")
  set(ACTUAL_INCLUDES)
  foreach(INCLUDE_LINE IN LISTS INCLUDE_LINES)
    if(NOT INCLUDE_LINE MATCHES "^#[ \t]*include[ \t]+\"([^\"]+)\"")
      message(FATAL_ERROR "cannot parse project include: ${FILE_PATH}: ${INCLUDE_LINE}")
    endif()
    list(APPEND ACTUAL_INCLUDES "${CMAKE_MATCH_1}")
  endforeach()
  set(EXPECTED_INCLUDES ${ARGN})
  assert_same_inventory(ACTUAL_INCLUDES EXPECTED_INCLUDES "${FILE_PATH} project include")
endfunction()

set(ALLOWED_SLOW_STANDARD_HEADERS
    algorithm array bit cmath cstddef cstdint cstdlib deque exception functional
    iostream limits map optional random set span sstream stdexcept string string_view
    tuple type_traits unordered_map unordered_set utility variant vector)
set(SLOW_ORACLE_FILES
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracle.hpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleInternal.hpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleTable.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleTransfer.cpp"
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOraclePropagation.cpp")
assert_quote_includes("${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracle.hpp" CsrReferenceTypes.hpp)
assert_quote_includes(
    "${CSR_TEST_ROOT}/CsrCpuReferenceSlowOracleInternal.hpp" CsrCpuReferenceSlowOracle.hpp)
foreach(SLOW_SOURCE IN ITEMS
        CsrCpuReferenceSlowOracleTable.cpp
        CsrCpuReferenceSlowOracleTransfer.cpp
        CsrCpuReferenceSlowOraclePropagation.cpp)
  assert_quote_includes(
      "${CSR_TEST_ROOT}/${SLOW_SOURCE}" CsrCpuReferenceSlowOracleInternal.hpp)
endforeach()
foreach(SLOW_FILE IN LISTS SLOW_ORACLE_FILES)
  file(STRINGS "${SLOW_FILE}" STANDARD_LINES REGEX "^#[ \t]*include[ \t]+<")
  foreach(STANDARD_LINE IN LISTS STANDARD_LINES)
    if(NOT STANDARD_LINE MATCHES "^#[ \t]*include[ \t]+<([^>]+)>")
      message(FATAL_ERROR "cannot parse standard include: ${SLOW_FILE}: ${STANDARD_LINE}")
    endif()
    list(FIND ALLOWED_SLOW_STANDARD_HEADERS "${CMAKE_MATCH_1}" STANDARD_INDEX)
    if(STANDARD_INDEX EQUAL -1)
      message(FATAL_ERROR "slow-oracle standard include is not allowed: ${CMAKE_MATCH_1}")
    endif()
  endforeach()
endforeach()

assert_quote_includes(
    "${CORE_DIR}/CsrCpuReferenceBackend.hpp"
    ActiveCsrEdgeView.hpp
    CsrFifoLevelization.hpp
    CsrReferenceTypes.hpp
    TimingPayloadView.hpp)
assert_quote_includes("${CORE_DIR}/CsrTableLookup.hpp" CsrReferenceNativeModels.hpp)

set(OUTCOME_TEST "${CSR_TEST_ROOT}/CsrCpuReferenceOutcomeContract.cpp")
foreach(STEP5_TEST_FILE IN LISTS EXPECTED_TEST_FILES)
  if(STEP5_TEST_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    continue()
  elseif(STEP5_TEST_FILE STREQUAL OUTCOME_TEST)
    read_required(OUTCOME_CONTENT "${STEP5_TEST_FILE}")
    require_literal_count(
        OUTCOME_CONTENT CsrCpuReferenceBackendInternal.hpp 1 "outcome internal-header include")
  else()
    require_absent("${STEP5_TEST_FILE}" CsrCpuReferenceBackendInternal.hpp
                   "non-outcome Step 5 test")
  endif()
endforeach()
set(TABLE_HEADER "${CORE_DIR}/CsrTableLookup.hpp")
set(TABLE_SOURCE "${CORE_DIR}/CsrTableLookup.cpp")
set(BACKEND_HEADER "${CORE_DIR}/CsrCpuReferenceBackend.hpp")
set(BACKEND_SOURCE "${CORE_DIR}/CsrCpuReferenceBackend.cpp")
set(BACKEND_INTERNAL "${CORE_DIR}/CsrCpuReferenceBackendInternal.hpp")
read_required(TABLE_HEADER_CONTENT "${TABLE_HEADER}")
read_required(TABLE_SOURCE_CONTENT "${TABLE_SOURCE}")
read_required(BACKEND_HEADER_CONTENT "${BACKEND_HEADER}")
read_required(BACKEND_SOURCE_CONTENT "${BACKEND_SOURCE}")
read_required(BACKEND_INTERNAL_CONTENT "${BACKEND_INTERNAL}")
require_literal_count(TABLE_HEADER_CONTENT "lookupCsrTimingTable(" 1 "table public declaration")
require_literal_count(TABLE_SOURCE_CONTENT "lookupCsrTimingTable(" 1 "table production definition")
require_literal_count(BACKEND_HEADER_CONTENT "CsrComputationResult compute(" 3 "backend public declarations")
require_literal_count(BACKEND_SOURCE_CONTENT "CsrCpuReferenceBackend::compute(" 3 "backend public definitions")
require_literal_count(BACKEND_INTERNAL_CONTENT "computeCsrCpuReferenceWithReplay(" 2 "backend seam declarations")
set(EXPECTED_WRAPPER [=[CsrComputationResult CsrCpuReferenceBackend::compute(const CsrReferenceInput& input) const
{
  if (!validateReferenceInput(input).empty())
    return CsrComputationResult{CsrComputationCode::kInvariantViolation};
  return computeWithTelemetry(input).computation;
}]=])
require_literal_count(BACKEND_SOURCE_CONTENT "${EXPECTED_WRAPPER}" 1 "backend exact wrapper")
set(ALL_PRODUCTION_CONTENT)
foreach(PRODUCTION_FILE IN LISTS EXPECTED_PRODUCTION_FILES)
  read_required(PRODUCTION_CONTENT "${PRODUCTION_FILE}")
  string(APPEND ALL_PRODUCTION_CONTENT "${PRODUCTION_CONTENT}")
  foreach(PROHIBITED_TOKEN
          "validateTimingResult(" "validateComputationResult("
          CsrReferenceCanonicalEncoder canonicalReferenceInputBytes
          buildReferenceFingerprintPayloads makeReferenceInputIdentity
          CsrNativeGraphOracle CsrCaptureMetadata CsrNativeTimingOracle TimingResultScatter CsrNativeSemanticKeyTestSupport timing_csr_snapshot "/scatter/"
          Database.hpp TimingPoint TimingArc.hpp TimingTable.hpp DelayCalculator
          ClockPropagator TimingPropagator TimingAnalyzer STAInterface
          "std::queue" "std::deque" indegree in_degree Kahn)
    require_absent("${PRODUCTION_FILE}" "${PROHIBITED_TOKEN}" "Step 5 production")
  endforeach()
endforeach()
require_literal_count(ALL_PRODUCTION_CONTENT "validateReferenceInput(" 3 "production input-validator calls")
require_literal_count(
    ALL_PRODUCTION_CONTENT "&replayFifoLevelization" 1 "Step 5 production FIFO address-take")
require_literal_count(
    ALL_PRODUCTION_CONTENT "replayFifoLevelization(" 0 "Step 5 production direct FIFO calls")
string(FIND "${BACKEND_SOURCE_CONTENT}" "validateReferenceInput(input)" VALIDATE_POSITION)
string(FIND "${BACKEND_SOURCE_CONTENT}" "input.unsupported_reasons" SUPPORT_POSITION)
string(FIND "${BACKEND_SOURCE_CONTENT}" "replay(input)" REPLAY_POSITION)
if(VALIDATE_POSITION EQUAL -1
   OR SUPPORT_POSITION LESS VALIDATE_POSITION
   OR REPLAY_POSITION LESS SUPPORT_POSITION)
  message(FATAL_ERROR "backend ordering must be validate -> support -> replay")
endif()
set(INIT_SOURCE "${CORE_DIR}/CsrCpuReferenceResultInitialization.cpp")
set(ARC_SOURCE "${CORE_DIR}/CsrCpuReferenceArcTransfer.cpp")
set(FORWARD_SOURCE "${CORE_DIR}/CsrCpuReferenceForwardPropagation.cpp")
set(REQUIRED_SOURCE "${CORE_DIR}/CsrCpuReferenceRequiredPropagation.cpp")
foreach(PROHIBITED_TOKEN
        referenced_library_cells instances cell_pin_capacitances port_constraints
        parasitics analysis_context ordered_end_ids endpoint_required)
  require_absent("${INIT_SOURCE}" "${PROHIBITED_TOKEN}" "result initialization allow-list")
endforeach()
foreach(PROHIBITED_TOKEN
        referenced_library_cells instances cell_pin_capacitances port_constraints
        parasitics analysis_context ordered_start_ids ordered_end_ids start_seeds
        clock_tags clocks endpoint_required)
  require_absent("${ARC_SOURCE}" "${PROHIBITED_TOKEN}" "arc transfer allow-list")
endforeach()
foreach(PROHIBITED_TOKEN
        referenced_library_cells instances cell_pin_capacitances port_constraints
        parasitics analysis_context start_seeds clock_tags clocks endpoint_required)
  require_absent("${FORWARD_SOURCE}" "${PROHIBITED_TOKEN}" "forward allow-list")
endforeach()
foreach(PROHIBITED_TOKEN
        referenced_library_cells instances cell_pin_capacitances port_constraints
        parasitics analysis_context start_seeds clocks)
  require_absent("${REQUIRED_SOURCE}" "${PROHIBITED_TOKEN}" "required allow-list")
endforeach()
foreach(PROHIBITED_TOKEN
        CsrReferenceInput unsupported_reasons analysis_context nodes arcs clocks
        port_constraints parasitics)
  require_absent("${TABLE_SOURCE}" "${PROHIBITED_TOKEN}" "table lookup allow-list")
endforeach()

foreach(SLOW_FILE IN LISTS SLOW_ORACLE_FILES)
  foreach(PROHIBITED_TOKEN
          CsrCpuReferenceBackend lookupCsrTimingTable replayFifoLevelization
          CsrFifoLevelization validateReferenceInput validateTimingResult
          validateComputationResult CsrReferenceCanonicalEncoder canonicalReferenceInputBytes
          CsrFingerprint CsrNativeGraphOracle CsrCaptureMetadata CsrNativeTimingOracle TimingResultScatter CsrNativeSemanticKeyTestSupport)
    require_absent("${SLOW_FILE}" "${PROHIBITED_TOKEN}" "independent slow oracle")
  endforeach()
endforeach()
set(PROPERTY_TEST "${CSR_TEST_ROOT}/CsrCpuReferencePropertyTest.cpp")
read_required(PROPERTY_CONTENT "${PROPERTY_TEST}")
require_literal_count(PROPERTY_CONTENT "requireExactComputationResult(first, second," 1 "property deterministic exact comparison")
require_literal_count(PROPERTY_CONTENT "requireExactComputationResult(first, oracle," 1 "property oracle exact computation comparison")
require_literal_count(PROPERTY_CONTENT "requireExactTimingResult(*timing, *oracle_timing," 1 "property oracle exact timing comparison")
set(PROHIBITED_PROPERTY_RESULT_EQUALITY_REGEXES
    "(^|[^A-Za-z0-9_])first[ \\t\\r\\n]*==[ \\t\\r\\n]*second([^A-Za-z0-9_]|$)" "(^|[^A-Za-z0-9_])second[ \\t\\r\\n]*==[ \\t\\r\\n]*first([^A-Za-z0-9_]|$)"
    "(^|[^A-Za-z0-9_])first[ \\t\\r\\n]*==[ \\t\\r\\n]*oracle([^A-Za-z0-9_]|$)" "(^|[^A-Za-z0-9_])oracle[ \\t\\r\\n]*==[ \\t\\r\\n]*first([^A-Za-z0-9_]|$)"
    "(^|[^A-Za-z0-9_])second[ \\t\\r\\n]*==[ \\t\\r\\n]*oracle([^A-Za-z0-9_]|$)" "(^|[^A-Za-z0-9_])oracle[ \\t\\r\\n]*==[ \\t\\r\\n]*second([^A-Za-z0-9_]|$)")
foreach(PROHIBITED_REGEX IN LISTS PROHIBITED_PROPERTY_RESULT_EQUALITY_REGEXES)
  string(REGEX MATCH "${PROHIBITED_REGEX}" PROHIBITED_MATCH "${PROPERTY_CONTENT}")
  if(NOT "${PROHIBITED_MATCH}" STREQUAL "")
    message(FATAL_ERROR "property result operands use prohibited operator==: ${PROHIBITED_MATCH}")
  endif()
endforeach()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 5 object isolation requires a Ninja generator")
endif()
require_file("${NINJA_EXECUTABLE}")
set(BUILD_NINJA "${CSR_BUILD_DIR}/build.ninja")
require_file("${BUILD_NINJA}")
file(READ "${BUILD_NINJA}" BUILD_NINJA_CONTENT)
execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
    RESULT_VARIABLE TARGET_RESULT
    OUTPUT_VARIABLE TARGET_OUTPUT
    ERROR_VARIABLE TARGET_ERROR)
if(NOT TARGET_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 5 Ninja targets: ${TARGET_ERROR}")
endif()
string(REPLACE "\n" ";" TARGET_LINES "${TARGET_OUTPUT}")

function(extract_object_rule OBJECT_PATH OUTPUT)
  set(RULE_TOKEN "build ${OBJECT_PATH}:")
  string(FIND "${BUILD_NINJA_CONTENT}" "${RULE_TOKEN}" RULE_POSITION)
  if(RULE_POSITION EQUAL -1)
    message(FATAL_ERROR "missing Step 5 Ninja object rule: ${OBJECT_PATH}")
  endif()
  string(SUBSTRING "${BUILD_NINJA_CONTENT}" ${RULE_POSITION} -1 RULE_TAIL)
  string(FIND "${RULE_TAIL}" "\n\n" RULE_END)
  if(RULE_END EQUAL -1)
    message(FATAL_ERROR "unterminated Step 5 Ninja object rule: ${OBJECT_PATH}")
  endif()
  string(SUBSTRING "${RULE_TAIL}" 0 ${RULE_END} RULE)
  set("${OUTPUT}" "${RULE}" PARENT_SCOPE)
endfunction()

function(require_strict_fp_rule OBJECT_PATH SOURCE_PATH)
  extract_object_rule("${OBJECT_PATH}" RULE)
  string(FIND "${RULE}" " ${SOURCE_PATH} ||" SOURCE_POSITION)
  if(SOURCE_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 5 object/source owner mismatch: ${OBJECT_PATH} -> ${SOURCE_PATH}")
  endif()
  string(FIND "${RULE}" "-ffp-contract=off" CONTRACT_POSITION)
  if(CONTRACT_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 5 object lacks -ffp-contract=off: ${OBJECT_PATH}")
  endif()
  foreach(FAST_TOKEN
          -ffast-math -Ofast -ffp-contract=fast -fassociative-math
          -funsafe-math-optimizations -freciprocal-math)
    string(FIND "${RULE}" "${FAST_TOKEN}" FAST_POSITION)
    if(NOT FAST_POSITION EQUAL -1)
      message(FATAL_ERROR "Step 5 object uses prohibited FP option ${FAST_TOKEN}: ${OBJECT_PATH}")
    endif()
  endforeach()
endfunction()
function(extract_project_cpp_deps CONTENT OUTPUT)
  string(REPLACE "\n" ";" DEP_LINES "${CONTENT}")
  foreach(DEP IN LISTS DEP_LINES)
    string(STRIP "${DEP}" DEP)
    cmake_path(NORMAL_PATH DEP)
    if(DEP MATCHES "\\.cpp$")
      list(APPEND CPP_DEPS "${DEP}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES CPP_DEPS)
  set("${OUTPUT}" "${CPP_DEPS}" PARENT_SCOPE)
endfunction()
cmake_path(GET CSR_BUILD_DIR PARENT_PATH PROJECT_ROOT)
extract_project_cpp_deps("${CSR_TEST_ROOT}/CsrTableLookupContractTest.cpp" SAFE_CPP_DEPS)
extract_project_cpp_deps("${CSR_TEST_ROOT}/CsrTableLookupContractTest.cpp\n${PROJECT_ROOT}/evaluation/src/util/init_contest_eval.cpp" POISON_CPP_DEPS)
if(NOT "${SAFE_CPP_DEPS}" STREQUAL "${CSR_TEST_ROOT}/CsrTableLookupContractTest.cpp" OR NOT "${POISON_CPP_DEPS}" STREQUAL "${CSR_TEST_ROOT}/CsrTableLookupContractTest.cpp;${PROJECT_ROOT}/evaluation/src/util/init_contest_eval.cpp")
  message(FATAL_ERROR "Step 5 depfile source-owner poison self-test failed")
endif()
function(read_valid_deps OBJECT_PATH SOURCE_PATH OUTPUT)
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${OBJECT_PATH}"
      RESULT_VARIABLE DEPS_RESULT
      OUTPUT_VARIABLE DEPS_OUTPUT
      ERROR_VARIABLE DEPS_ERROR)
  if(NOT DEPS_RESULT EQUAL 0
     OR NOT DEPS_OUTPUT MATCHES "\\(VALID\\)")
    message(FATAL_ERROR
            "missing valid Step 5 depfile for ${OBJECT_PATH}: ${DEPS_OUTPUT}${DEPS_ERROR}")
  endif()
  extract_project_cpp_deps("${DEPS_OUTPUT}" CPP_DEPS)
  cmake_path(NORMAL_PATH SOURCE_PATH)
  if(NOT "${CPP_DEPS}" STREQUAL "${SOURCE_PATH}")
    message(FATAL_ERROR "Step 5 depfile must have exact singleton .cpp owner ${SOURCE_PATH}: actual=[${CPP_DEPS}]: ${OBJECT_PATH}")
  endif()
  set("${OUTPUT}" "${DEPS_OUTPUT}" PARENT_SCOPE)
endfunction()

set(TARGET_ROWS
    "ista_csr_table_lookup_contract_test=${EXPECTED_TABLE_SOURCES_PACKED}"
    "ista_csr_cpu_reference_contract_test=${EXPECTED_CONTRACT_SOURCES_PACKED}"
    "ista_csr_cpu_reference_property_test=${EXPECTED_PROPERTY_SOURCES_PACKED}"
    "ista_csr_cpu_reference_public_headers_test=${EXPECTED_PUBLIC_HEADER_SOURCES_PACKED}")
set(ALL_EXPECTED_TEST_OBJECTS)
set(SLOW_OBJECTS)
foreach(TARGET_ROW IN LISTS TARGET_ROWS)
  string(FIND "${TARGET_ROW}" "=" SEPARATOR)
  string(SUBSTRING "${TARGET_ROW}" 0 ${SEPARATOR} TARGET_NAME)
  math(EXPR SOURCE_OFFSET "${SEPARATOR} + 1")
  string(SUBSTRING "${TARGET_ROW}" ${SOURCE_OFFSET} -1 SOURCE_PACKED)
  string(REPLACE "|" ";" TARGET_SOURCES "${SOURCE_PACKED}")
  set(OBJECT_PREFIX "src/operation/iSTA/test/CMakeFiles/${TARGET_NAME}.dir/")
  set(EXPECTED_TARGET_OBJECTS)
  foreach(TARGET_SOURCE IN LISTS TARGET_SOURCES)
    set(OBJECT_PATH "${OBJECT_PREFIX}${TARGET_SOURCE}.o")
    set(SOURCE_PATH "${CSR_TEST_ROOT}/${TARGET_SOURCE}")
    list(APPEND EXPECTED_TARGET_OBJECTS "${OBJECT_PATH}")
    list(APPEND ALL_EXPECTED_TEST_OBJECTS "${OBJECT_PATH}")
    require_strict_fp_rule("${OBJECT_PATH}" "${SOURCE_PATH}")
    read_valid_deps("${OBJECT_PATH}" "${SOURCE_PATH}" DEPS)
    string(FIND "${DEPS}" CsrCaptureMetadata.hpp METADATA_POSITION)
    if(NOT METADATA_POSITION EQUAL -1)
      message(FATAL_ERROR "Step 5 object depends on capture metadata: ${OBJECT_PATH}")
    endif()
    string(FIND "${DEPS}" CsrCpuReferenceBackendInternal.hpp INTERNAL_POSITION)
    if(TARGET_SOURCE STREQUAL CsrCpuReferenceOutcomeContract.cpp)
      if(INTERNAL_POSITION EQUAL -1)
        message(FATAL_ERROR "outcome object is missing backend internal seam dependency")
      endif()
    elseif(NOT INTERNAL_POSITION EQUAL -1)
      message(FATAL_ERROR "non-outcome object depends on backend internal seam: ${OBJECT_PATH}")
    endif()
    if(TARGET_SOURCE MATCHES "^CsrCpuReferenceSlowOracle.*\\.cpp$")
      list(APPEND SLOW_OBJECTS "${OBJECT_PATH}")
      foreach(PROHIBITED_DEP
              CsrCpuReferenceBackend.hpp CsrCpuReferenceBackendInternal.hpp
              CsrTableLookup.hpp CsrFifoLevelization.hpp CsrReferenceValidator.hpp
              CsrReferenceCanonicalEncoder.hpp CsrFingerprint.hpp
              CsrNativeGraphOracle.hpp CsrCaptureMetadata.hpp CsrNativeTimingOracle.hpp TimingResultScatter.hpp CsrNativeSemanticKeyTestSupport.hpp)
        string(FIND "${DEPS}" "${PROHIBITED_DEP}" DEP_POSITION)
        if(NOT DEP_POSITION EQUAL -1)
          message(FATAL_ERROR "slow-oracle depfile escape ${PROHIBITED_DEP}: ${OBJECT_PATH}")
        endif()
      endforeach()
    endif()
  endforeach()
  set(ACTUAL_TARGET_OBJECTS)
  foreach(TARGET_LINE IN LISTS TARGET_LINES)
    if(TARGET_LINE MATCHES "^(${OBJECT_PREFIX}[^:]+\\.cpp\\.o):")
      list(APPEND ACTUAL_TARGET_OBJECTS "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  assert_same_inventory(
      ACTUAL_TARGET_OBJECTS EXPECTED_TARGET_OBJECTS "${TARGET_NAME} Ninja object")
endforeach()
list(LENGTH ALL_EXPECTED_TEST_OBJECTS TEST_OBJECT_COUNT)
if(NOT TEST_OBJECT_COUNT EQUAL 16)
  message(FATAL_ERROR "Step 5 target object inventory must contain exactly 16 objects")
endif()
list(LENGTH SLOW_OBJECTS SLOW_OBJECT_COUNT)
if(NOT SLOW_OBJECT_COUNT EQUAL 3)
  message(FATAL_ERROR "Step 5 slow-oracle inventory must contain exactly 3 objects")
endif()

set(PRODUCTION_OBJECT_NAMES
    CsrTableLookup.cpp
    CsrCpuReferenceBackend.cpp
    CsrCpuReferenceResultInitialization.cpp
    CsrCpuReferenceArcTransfer.cpp
    CsrCpuReferenceForwardPropagation.cpp
    CsrCpuReferenceRequiredPropagation.cpp)
set(PRODUCTION_OBJECT_PREFIX
    "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/")
set(PROHIBITED_UNDEFINED_SYMBOLS
    validateTimingResult validateComputationResult canonicalReferenceInputBytes
    buildReferenceFingerprintPayloads makeReferenceInputIdentity
    CsrNativeGraphOracle CsrNativeTimingEvaluator CsrCaptureMetadata CsrSnapshot CsrNativeTimingOracle TimingResultScatter CsrNativeSemanticKeyTestSupport
    ClockPropagator TimingPropagator TimingAnalyzer STAInterface)
foreach(PRODUCTION_OBJECT_NAME IN LISTS PRODUCTION_OBJECT_NAMES)
  set(OBJECT_PATH "${PRODUCTION_OBJECT_PREFIX}${PRODUCTION_OBJECT_NAME}.o")
  set(SOURCE_PATH "${CORE_DIR}/${PRODUCTION_OBJECT_NAME}")
  require_strict_fp_rule("${OBJECT_PATH}" "${SOURCE_PATH}")
  read_valid_deps("${OBJECT_PATH}" "${SOURCE_PATH}" DEPS)
  foreach(PROHIBITED_DEP
          "/source/data_manager/advance/" "/source/module/clock_propagator/"
          "/source/module/timing_propagator/" "/source/module/timing_analyzer/"
          "/source/module/timing_csr_snapshot/" CsrNativeGraphOracle.hpp CsrNativeTimingOracle.hpp
          CsrCaptureMetadata.hpp TimingResultScatter.hpp CsrNativeSemanticKeyTestSupport.hpp STAInterface.hpp)
    string(FIND "${DEPS}" "${PROHIBITED_DEP}" DEP_POSITION)
    if(NOT DEP_POSITION EQUAL -1)
      message(FATAL_ERROR "production depfile escape ${PROHIBITED_DEP}: ${OBJECT_PATH}")
    endif()
  endforeach()
  set(ABSOLUTE_OBJECT "${CSR_BUILD_DIR}/${OBJECT_PATH}")
  require_file("${ABSOLUTE_OBJECT}")
  execute_process(
      COMMAND "${NM_EXECUTABLE}" -uC "${ABSOLUTE_OBJECT}"
      RESULT_VARIABLE NM_RESULT OUTPUT_VARIABLE NM_OUTPUT ERROR_VARIABLE NM_ERROR)
  if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot inspect production symbols for ${OBJECT_PATH}: ${NM_ERROR}")
  endif()
  foreach(PROHIBITED_SYMBOL IN LISTS PROHIBITED_UNDEFINED_SYMBOLS)
    string(FIND "${NM_OUTPUT}" "${PROHIBITED_SYMBOL}" SYMBOL_POSITION)
    if(NOT SYMBOL_POSITION EQUAL -1)
      message(FATAL_ERROR
              "prohibited production undefined symbol ${PROHIBITED_SYMBOL}: ${OBJECT_PATH}")
    endif()
  endforeach()
  if(PRODUCTION_OBJECT_NAME STREQUAL CsrCpuReferenceBackend.cpp)
    foreach(REQUIRED_SYMBOL validateReferenceInput replayFifoLevelization)
      string(FIND "${NM_OUTPUT}" "${REQUIRED_SYMBOL}" SYMBOL_POSITION)
      if(SYMBOL_POSITION EQUAL -1)
        message(FATAL_ERROR "backend is missing required undefined symbol ${REQUIRED_SYMBOL}")
      endif()
    endforeach()
  else()
    foreach(BACKEND_ONLY_SYMBOL validateReferenceInput replayFifoLevelization)
      string(FIND "${NM_OUTPUT}" "${BACKEND_ONLY_SYMBOL}" SYMBOL_POSITION)
      if(NOT SYMBOL_POSITION EQUAL -1)
        message(FATAL_ERROR
                "non-backend object references ${BACKEND_ONLY_SYMBOL}: ${OBJECT_PATH}")
      endif()
    endforeach()
  endif()
endforeach()

foreach(SLOW_OBJECT IN LISTS SLOW_OBJECTS)
  set(ABSOLUTE_OBJECT "${CSR_BUILD_DIR}/${SLOW_OBJECT}")
  require_file("${ABSOLUTE_OBJECT}")
  execute_process(
      COMMAND "${NM_EXECUTABLE}" -uC "${ABSOLUTE_OBJECT}"
      RESULT_VARIABLE NM_RESULT OUTPUT_VARIABLE NM_OUTPUT ERROR_VARIABLE NM_ERROR)
  if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot inspect slow-oracle symbols: ${SLOW_OBJECT}: ${NM_ERROR}")
  endif()
  foreach(PROHIBITED_SYMBOL
          CsrCpuReferenceBackend lookupCsrTimingTable replayFifoLevelization
          validateReferenceInput validateTimingResult validateComputationResult
          canonicalReferenceInputBytes CsrFingerprint CsrNativeGraphOracle CsrCaptureMetadata CsrNativeTimingOracle TimingResultScatter CsrNativeSemanticKeyTestSupport)
    string(FIND "${NM_OUTPUT}" "${PROHIBITED_SYMBOL}" SYMBOL_POSITION)
    if(NOT SYMBOL_POSITION EQUAL -1)
      message(FATAL_ERROR
              "slow-oracle undefined-symbol escape ${PROHIBITED_SYMBOL}: ${SLOW_OBJECT}")
    endif()
  endforeach()
endforeach()

file(GLOB_RECURSE SOURCE_ARTIFACTS LIST_DIRECTORIES TRUE
     "${CORE_DIR}/CsrCpuReference*" "${CORE_DIR}/CsrTableLookup*"
     "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/CsrReferenceTypes*"
     "${CSR_TEST_ROOT}/CsrCpuReference*" "${CSR_TEST_ROOT}/CsrTableLookup*")
foreach(SOURCE_ARTIFACT IN LISTS SOURCE_ARTIFACTS)
  if(SOURCE_ARTIFACT MATCHES
     "(__pycache__|\\.pyc$|\\.o$|\\.d$|\\.patch$|\\.rej$|\\.orig$|\\.bak$|~$|CMakeCache\\.txt$|build\\.ninja$)")
    message(FATAL_ERROR "Step 5 source-tree artifact leakage: ${SOURCE_ARTIFACT}")
  endif()
endforeach()

message(STATUS "CSR Step 5 source, object, depfile, symbol, and detector isolation passed")
