# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0012 NEW)
cmake_policy(SET CMP0130 NEW)

foreach(REQUIRED_VARIABLE
        CSR_ADAPTER_ALLOWED_FILES_PACKED
        CSR_ADAPTER_ACTUAL_SOURCES_PACKED
        CSR_ADAPTER_EXPECTED_SOURCES_PACKED
        CSR_ADAPTER_EXPECTED_OBJECTS_PACKED
        CSR_ADAPTER_OBJECT_DIR
        CSR_DIRECT_PROJECTOR_FILES_PACKED
        CSR_BUILD_DIR
        NINJA_EXECUTABLE
        ISTA_SOURCE_ROOT
        CSR_TEST_ROOT
        CXX_COMPILER
        NM_EXECUTABLE
        CSR_GENERATOR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "CSR adapter compiler-dependency isolation requires a Ninja generator")
endif()
if(NOT EXISTS "${NINJA_EXECUTABLE}")
  message(FATAL_ERROR "Ninja executable is missing: ${NINJA_EXECUTABLE}")
endif()

foreach(PACKED_NAME
        CSR_ADAPTER_ALLOWED_FILES
        CSR_ADAPTER_ACTUAL_SOURCES
        CSR_ADAPTER_EXPECTED_SOURCES
        CSR_ADAPTER_EXPECTED_OBJECTS
        CSR_DIRECT_PROJECTOR_FILES)
  string(REPLACE "|" ";" ${PACKED_NAME} "${${PACKED_NAME}_PACKED}")
endforeach()

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
    message(
        FATAL_ERROR
        "${DESCRIPTION} inventory mismatch; actual=[${ACTUAL_SORTED}] expected=[${EXPECTED_SORTED}]"
    )
  endif()
endfunction()

execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
    RESULT_VARIABLE TARGETS_RESULT
    OUTPUT_VARIABLE TARGETS_OUTPUT
    ERROR_VARIABLE TARGETS_ERROR
)
if(NOT TARGETS_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot read Ninja target inventory: ${TARGETS_ERROR}")
endif()
set(CSR_ADAPTER_ACTUAL_OBJECTS)
string(REPLACE "\n" ";" TARGET_LINES "${TARGETS_OUTPUT}")
foreach(TARGET_LINE IN LISTS TARGET_LINES)
  if(TARGET_LINE MATCHES "^([^:]+\\.cpp\\.o):")
    set(CANDIDATE "${CMAKE_MATCH_1}")
    string(FIND "${CANDIDATE}" "${CSR_ADAPTER_OBJECT_DIR}/" OBJECT_PREFIX)
    if(OBJECT_PREFIX EQUAL 0)
      list(APPEND CSR_ADAPTER_ACTUAL_OBJECTS "${CANDIDATE}")
    endif()
  endif()
endforeach()

assert_same_inventory(
    CSR_ADAPTER_ACTUAL_SOURCES CSR_ADAPTER_EXPECTED_SOURCES "CSR adapter source"
)
assert_same_inventory(
    CSR_ADAPTER_ACTUAL_OBJECTS CSR_ADAPTER_EXPECTED_OBJECTS "CSR adapter object"
)

list(LENGTH CSR_ADAPTER_EXPECTED_SOURCES SOURCE_COUNT)
list(LENGTH CSR_ADAPTER_EXPECTED_OBJECTS OBJECT_COUNT)
if(NOT SOURCE_COUNT EQUAL 18 OR NOT OBJECT_COUNT EQUAL 18)
  message(FATAL_ERROR "CSR adapter inventory must contain exactly 18 sources and objects")
endif()
if(NOT SOURCE_COUNT EQUAL OBJECT_COUNT)
  message(FATAL_ERROR "CSR adapter source/object counts differ: ${SOURCE_COUNT} vs ${OBJECT_COUNT}")
endif()

foreach(SOURCE_FILE IN LISTS CSR_ADAPTER_EXPECTED_SOURCES)
  if(NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "missing CSR adapter source: ${SOURCE_FILE}")
  endif()
  get_filename_component(SOURCE_NAME "${SOURCE_FILE}" NAME)
  set(MATCH_COUNT 0)
  foreach(OBJECT_FILE IN LISTS CSR_ADAPTER_EXPECTED_OBJECTS)
    string(FIND "${OBJECT_FILE}" "/${SOURCE_NAME}.o" OBJECT_SUFFIX REVERSE)
    if(NOT OBJECT_SUFFIX EQUAL -1)
      math(EXPR MATCH_COUNT "${MATCH_COUNT} + 1")
    endif()
  endforeach()
  if(NOT MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "source must map to exactly one Ninja object: ${SOURCE_FILE}")
  endif()
endforeach()

foreach(OBJECT_FILE IN LISTS CSR_ADAPTER_EXPECTED_OBJECTS)
  set(MATCH_COUNT 0)
  foreach(SOURCE_FILE IN LISTS CSR_ADAPTER_EXPECTED_SOURCES)
    get_filename_component(SOURCE_NAME "${SOURCE_FILE}" NAME)
    string(FIND "${OBJECT_FILE}" "/${SOURCE_NAME}.o" OBJECT_SUFFIX REVERSE)
    if(NOT OBJECT_SUFFIX EQUAL -1)
      math(EXPR MATCH_COUNT "${MATCH_COUNT} + 1")
    endif()
  endforeach()
  if(NOT MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "Ninja object must map to exactly one adapter source: ${OBJECT_FILE}")
  endif()
endforeach()

set(PROHIBITED_DEPENDENCY_FRAGMENTS
    "/DataManager.hpp"
    "/interface/"
    "/module/graph_builder/"
    "/module/delay_calculator/"
    "/module/clock_propagator/"
    "/module/timing_propagator/"
    "/module/timing_analyzer/"
    "/backend/"
    "/scatter/"
    "/CsrNativeTimingOracle.hpp"
    "/CsrNativeSemanticKeyTestSupport.hpp"
)

foreach(ADAPTER_SOURCE IN LISTS CSR_ADAPTER_EXPECTED_SOURCES)
  file(READ "${ADAPTER_SOURCE}" ADAPTER_CONTENT)
  foreach(TEST_ONLY_HEADER IN ITEMS
          CsrNativeTimingOracle.hpp CsrNativeSemanticKeyTestSupport.hpp)
    string(FIND "${ADAPTER_CONTENT}" "${TEST_ONLY_HEADER}" HEADER_POSITION)
    if(NOT HEADER_POSITION EQUAL -1)
      message(FATAL_ERROR
              "test-only CSR header leaked into adapter source ${ADAPTER_SOURCE}: ${TEST_ONLY_HEADER}")
    endif()
  endforeach()
endforeach()

set(BRIDGE_OBJECT
    "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrSessionSnapshotAdapter.cpp.o")
set(SNAPSHOT_PUBLISH_OBJECT
    "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatterPublish.cpp.o")
set(BRIDGE_SNAPSHOT_DEPENDENCIES)
set(PUBLISH_SNAPSHOT_DEPENDENCIES)
set(EXPECTED_BRIDGE_SNAPSHOT_DEPENDENCIES
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotObservation.hpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotObservationTransition.hpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotObservationValidator.hpp"
)
set(EXPECTED_PUBLISH_SNAPSHOT_DEPENDENCIES
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CsrSnapshotObservation.hpp"
)

foreach(ADAPTER_OBJECT IN LISTS CSR_ADAPTER_EXPECTED_OBJECTS)
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${ADAPTER_OBJECT}"
      RESULT_VARIABLE DEPS_RESULT
      OUTPUT_VARIABLE DEPS_OUTPUT
      ERROR_VARIABLE DEPS_ERROR
  )
  if(NOT DEPS_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read compiler dependencies for ${ADAPTER_OBJECT}: ${DEPS_ERROR}")
  endif()
  string(FIND "${DEPS_OUTPUT}" "(VALID)" VALID_POSITION)
  if(VALID_POSITION EQUAL -1)
    message(FATAL_ERROR "compiler dependencies are missing or stale for ${ADAPTER_OBJECT}")
  endif()
  string(REPLACE "\n" ";" DEPENDENCY_LINES "${DEPS_OUTPUT}")
  foreach(DEPENDENCY_LINE IN LISTS DEPENDENCY_LINES)
    string(STRIP "${DEPENDENCY_LINE}" DEPENDENCY)
    string(FIND "${DEPENDENCY}" "${ISTA_SOURCE_ROOT}/" SOURCE_PREFIX)
    if(SOURCE_PREFIX EQUAL 0)
      string(
          FIND "${DEPENDENCY}" "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/"
          SNAPSHOT_DEPENDENCY_POSITION
      )
      if(NOT SNAPSHOT_DEPENDENCY_POSITION EQUAL -1)
        if(ADAPTER_OBJECT STREQUAL BRIDGE_OBJECT)
          list(APPEND BRIDGE_SNAPSHOT_DEPENDENCIES "${DEPENDENCY}")
        elseif(ADAPTER_OBJECT STREQUAL SNAPSHOT_PUBLISH_OBJECT)
          list(APPEND PUBLISH_SNAPSHOT_DEPENDENCIES "${DEPENDENCY}")
        else()
          message(
              FATAL_ERROR
              "only the session bridge and no-fail publish objects may depend on snapshot public headers: ${ADAPTER_OBJECT}: ${DEPENDENCY}"
          )
        endif()
      endif()
      foreach(PROHIBITED_FRAGMENT IN LISTS PROHIBITED_DEPENDENCY_FRAGMENTS)
        string(FIND "${DEPENDENCY}" "${PROHIBITED_FRAGMENT}" PROHIBITED_POSITION)
        if(NOT PROHIBITED_POSITION EQUAL -1)
          message(FATAL_ERROR "CSR adapter dependency is prohibited in ${ADAPTER_OBJECT}: ${DEPENDENCY}")
        endif()
      endforeach()
      list(FIND CSR_ADAPTER_ALLOWED_FILES "${DEPENDENCY}" ALLOWED_INDEX)
      if(ALLOWED_INDEX EQUAL -1)
        message(
            FATAL_ERROR
            "CSR adapter compiler dependency escapes the exact allow-list in ${ADAPTER_OBJECT}: ${DEPENDENCY}"
        )
      endif()
    endif()
  endforeach()
endforeach()
assert_same_inventory(
    BRIDGE_SNAPSHOT_DEPENDENCIES EXPECTED_BRIDGE_SNAPSHOT_DEPENDENCIES
    "session bridge snapshot public-header dependency"
)
assert_same_inventory(
    PUBLISH_SNAPSHOT_DEPENDENCIES EXPECTED_PUBLISH_SNAPSHOT_DEPENDENCIES
    "no-fail publish snapshot public-header dependency"
)

set(PROHIBITED_DIRECT_PROJECTOR_TOKENS
    "exportCsrGraphSnapshot"
    "decodeNativeGraphProjection"
    "assignParallelOrdinals"
    "TimingGraphCsrAdapter.hpp"
    "CsrGraphValidator.hpp"
)
foreach(PROJECTOR_FILE IN LISTS CSR_DIRECT_PROJECTOR_FILES)
  if(NOT EXISTS "${PROJECTOR_FILE}")
    message(FATAL_ERROR "missing direct-projector file: ${PROJECTOR_FILE}")
  endif()
  file(READ "${PROJECTOR_FILE}" PROJECTOR_CONTENT)
  foreach(PROHIBITED_TOKEN IN LISTS PROHIBITED_DIRECT_PROJECTOR_TOKENS)
    string(FIND "${PROJECTOR_CONTENT}" "${PROHIBITED_TOKEN}" TOKEN_POSITION)
    if(NOT TOKEN_POSITION EQUAL -1)
      message(FATAL_ERROR "direct projector is coupled to production builder: ${PROJECTOR_FILE}: ${PROHIBITED_TOKEN}")
    endif()
  endforeach()
endforeach()
function(require_absent FILE_PATH TOKEN DESCRIPTION)
  file(READ "${FILE_PATH}" CONTENT)
  string(FIND "${CONTENT}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR
            "${DESCRIPTION} contains prohibited token ${TOKEN}: ${FILE_PATH}")
  endif()
endfunction()
function(require_present FILE_PATH TOKEN DESCRIPTION)
  file(READ "${FILE_PATH}" CONTENT)
  string(FIND "${CONTENT}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR
            "${DESCRIPTION} is missing required token ${TOKEN}: ${FILE_PATH}")
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
    message(FATAL_ERROR
            "${DESCRIPTION} count mismatch: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

set(ADAPTER_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr")
set(ADVANCE_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/advance")
set(ORCHESTRATOR "${ADAPTER_DIR}/TimingResultScatter.cpp")
set(VALIDATION "${ADAPTER_DIR}/TimingResultScatterValidation.cpp")
set(PREPARATION "${ADAPTER_DIR}/TimingResultScatterPreparation.cpp")
set(PUBLISH "${ADAPTER_DIR}/TimingResultScatterPublish.cpp")
foreach(SCATTER_FILE IN ITEMS "${ORCHESTRATOR}" "${VALIDATION}" "${PREPARATION}"
                              "${PUBLISH}")
  if(NOT EXISTS "${SCATTER_FILE}")
    message(FATAL_ERROR "missing scatter source: ${SCATTER_FILE}")
  endif()
endforeach()
file(READ "${PUBLISH}" PUBLISH_CONTENT)
require_literal_count(PUBLISH_CONTENT "static_assert(std::is_nothrow_swappable_v<" 10 "publish swappable traits")
require_literal_count(PUBLISH_CONTENT "static_assert(noexcept(" 28 "publish noexcept family traits")
require_literal_count(PUBLISH_CONTENT "node.target->" 11 "publish node direct families")
require_literal_count(PUBLISH_CONTENT "target.get_" 10 "publish clock-map families")
require_literal_count(PUBLISH_CONTENT "arc.target->" 8 "publish arc families")
require_literal_count(PUBLISH_CONTENT "timing_order_target->swap(" 1 "publish timing order family")
require_literal_count(PUBLISH_CONTENT "publishClockState(" 3 "publish clock helper definition/calls")
require_literal_count(PUBLISH_CONTENT "arc.target->_timing_arc_delay_state.swap(" 1
                      "private-friend timing-arc state swap")
require_literal_count(PUBLISH_CONTENT "replace_timing_arc_delay_state(" 0
                      "public normalization replacement in publish")
foreach(PREPUBLISH_FILE IN ITEMS "${VALIDATION}" "${PREPARATION}")
  foreach(TOKEN "TimingPoint::set_" "Arc::set_" "replace_timing_arc_delay_state" "target->_timing_arc_delay_state")
    require_absent("${PREPUBLISH_FILE}" "${TOKEN}" "prepublish native mutation")
  endforeach()
endforeach()
foreach(TOKEN "operator new" "new " "malloc(" "calloc(" "realloc("
              ".find(" ".at(" "operator[" "unordered_" "std::hash"
              validate exportCsr Logger callback push_back emplace insert erase reserve resize)
  require_absent("${PUBLISH}" "${TOKEN}" "no-fail publish")
endforeach()
foreach(SCATTER_FILE IN ITEMS "${ORCHESTRATOR}" "${VALIDATION}" "${PREPARATION}")
  foreach(TOKEN "target->set_" "target->get_" "timing_order_target->swap("
                "replace_timing_arc_delay_state(" "target->_timing_arc_delay_state")
    require_absent("${SCATTER_FILE}" "${TOKEN}" "publish-only native mutation")
  endforeach()
endforeach()
file(GLOB ADAPTER_CPP LIST_DIRECTORIES FALSE "${ADAPTER_DIR}/*.cpp")
set(NATIVE_MUTATION_FILES)
foreach(ADAPTER_CPP_FILE IN LISTS ADAPTER_CPP)
  file(STRINGS "${ADAPTER_CPP_FILE}" MUTATION_LINES
       REGEX "(target->set_|target->get_.*\\.swap\\(|target->_timing_arc_delay_state\\.swap\\(|timing_order_target->swap\\(|replace_timing_arc_delay_state\\()")
  if(MUTATION_LINES)
    list(APPEND NATIVE_MUTATION_FILES "${ADAPTER_CPP_FILE}")
  endif()
endforeach()
set(EXPECTED_NATIVE_MUTATION_FILES "${PUBLISH}")
assert_same_inventory(NATIVE_MUTATION_FILES EXPECTED_NATIVE_MUTATION_FILES
                      "publish-only native timing mutation source")

file(READ "${ORCHESTRATOR}" ORCHESTRATOR_CONTENT)
require_literal_count(ORCHESTRATOR_CONTENT
                      "CsrTimingResultScatterAttempt success_attempt" 1
                      "complete non-const success attempt")
require_literal_count(ORCHESTRATOR_CONTENT
                      "detail::publishPreparedTimingResult(envelope.prepared);" 1
                      "unique publish call")
set(EXACT_PUBLISH_TAIL [=[detail::publishPreparedTimingResult(envelope.prepared);
  return success_attempt;
}]=])
require_literal_count(ORCHESTRATOR_CONTENT "${EXACT_PUBLISH_TAIL}" 1
                      "orchestrator plain-return tail")
foreach(TRAIT
        "is_nothrow_move_constructible_v<CsrTimingResultScatterAttempt>"
        "is_nothrow_destructible_v<detail::PreparedClockState>"
        "is_nothrow_destructible_v<detail::PreparedTimingPointState>"
        "is_nothrow_destructible_v<detail::PreparedArcState>"
        "is_nothrow_destructible_v<detail::PreparedTimingResult>"
        "is_nothrow_destructible_v<detail::PreparedScatterEnvelope>")
  require_literal_count(ORCHESTRATOR_CONTENT "${TRAIT}" 1
                        "orchestrator lifetime trait")
endforeach()

set(ADAPTER_OBJECT_PREFIX
    "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/")
foreach(NAME TimingResultScatterValidation.cpp TimingResultScatterPreparation.cpp)
  set(OBJECT "${CSR_BUILD_DIR}/${ADAPTER_OBJECT_PREFIX}${NAME}.o")
  if(NOT EXISTS "${OBJECT}")
    message(FATAL_ERROR "missing prepublish object: ${OBJECT}")
  endif()
  execute_process(
      COMMAND "${NM_EXECUTABLE}" -uC "${OBJECT}"
      RESULT_VARIABLE NM_RESULT OUTPUT_VARIABLE NM_OUTPUT ERROR_VARIABLE NM_ERROR)
  if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot inspect prepublish symbols: ${NM_ERROR}")
  endif()
  foreach(TOKEN "TimingPoint::set_" "Arc::set_" replace_timing_arc_delay_state)
    string(FIND "${NM_OUTPUT}" "${TOKEN}" POSITION)
    if(NOT POSITION EQUAL -1)
      message(FATAL_ERROR
              "prepublish object references native mutation ${TOKEN}: ${NAME}")
    endif()
  endforeach()
endforeach()
set(PUBLISH_OBJECT
    "${CSR_BUILD_DIR}/${ADAPTER_OBJECT_PREFIX}TimingResultScatterPublish.cpp.o")
if(NOT EXISTS "${PUBLISH_OBJECT}")
  message(FATAL_ERROR "missing publish object: ${PUBLISH_OBJECT}")
endif()
execute_process(
    COMMAND "${NM_EXECUTABLE}" -uC "${PUBLISH_OBJECT}"
    RESULT_VARIABLE NM_RESULT OUTPUT_VARIABLE PUBLISH_NM ERROR_VARIABLE NM_ERROR)
if(NOT NM_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect publish symbols: ${NM_ERROR}")
endif()
foreach(TOKEN "operator new" "operator delete" malloc free realloc calloc
              "std::__throw" "std::_Rb_tree" validate exportCsr Logger callback)
  string(FIND "${PUBLISH_NM}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR "publish object has prohibited symbol ${TOKEN}")
  endif()
endforeach()

function(run_strict_switch_compile OBJECT_PATH)
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t commands "${OBJECT_PATH}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE COMMAND_TEXT ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read strict compile command: ${ERROR}")
  endif()
  string(STRIP "${COMMAND_TEXT}" COMMAND_TEXT)
  separate_arguments(COMMAND_PARTS UNIX_COMMAND "${COMMAND_TEXT}")
  set(FILTERED)
  set(SKIP_NEXT FALSE)
  foreach(PART IN LISTS COMMAND_PARTS)
    if(SKIP_NEXT)
      set(SKIP_NEXT FALSE)
    elseif(PART STREQUAL "-MT" OR PART STREQUAL "-MF" OR PART STREQUAL "-o")
      set(SKIP_NEXT TRUE)
    elseif(PART STREQUAL "-MD" OR PART STREQUAL "-MMD")
    else()
      list(APPEND FILTERED "${PART}")
    endif()
  endforeach()
  execute_process(
      COMMAND ${FILTERED}
              -isystem "${ISTA_SOURCE_ROOT}/source/module/timing_csr"
              -isystem "${ISTA_SOURCE_ROOT}/source/data_manager/advance"
              -isystem "${ISTA_SOURCE_ROOT}/source/data_manager/basic"
              -isystem "${ISTA_SOURCE_ROOT}/source/toolkit/logger"
              -Wextra -Werror -pedantic -Wswitch-enum
              -ffp-contract=off -fsyntax-only
      RESULT_VARIABLE STRICT_RESULT OUTPUT_VARIABLE STRICT_OUT ERROR_VARIABLE STRICT_ERR)
  if(NOT STRICT_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Step 6 strict standalone compile failed: ${STRICT_OUT}${STRICT_ERR}")
  endif()
endfunction()
run_strict_switch_compile(
    "${ADAPTER_OBJECT_PREFIX}TimingResultScatterValidation.cpp.o")
run_strict_switch_compile(
    "${ADAPTER_OBJECT_PREFIX}TimingResultScatterPreparation.cpp.o")

set(ARC_HEADER "${ADVANCE_DIR}/Arc.hpp")
file(READ "${ARC_HEADER}" ARC_CONTENT)
require_literal_count(ARC_CONTENT "get_timing_arc_delay_state() const noexcept" 1
                      "typed timing-arc state reader")
require_literal_count(ARC_CONTENT "get_timing_arc_delay_map() noexcept" 1
                      "mutable timing-arc escape hatch")
require_literal_count(ARC_CONTENT "get_timing_arc_delay_map() const noexcept" 1
                      "const timing-arc map reader")
require_literal_count(ARC_CONTENT
                      "friend void csr::detail::publishPreparedTimingResult(csr::detail::PreparedTimingResult&) noexcept;"
                      1 "exact private timing-arc publisher friend")
require_literal_count(ARC_CONTENT
                      "replacement.delay_map = timing_arc_delay_map;" 1
                      "detached legacy timing-arc map copy")
require_literal_count(ARC_CONTENT
                      "_timing_arc_delay_state.swap(replacement);" 1
                      "atomic legacy timing-arc state swap")
require_literal_count(ARC_CONTENT
                      "_timing_arc_delay_state.delay_map = timing_arc_delay_map;" 0
                      "non-atomic legacy timing-arc map assignment")
file(GLOB_RECURSE PRODUCTION_CPP LIST_DIRECTORIES FALSE
     "${ISTA_SOURCE_ROOT}/source/*.cpp")
set(PRODUCTION_DELAY_MAP_FILES)
foreach(PRODUCTION_FILE IN LISTS PRODUCTION_CPP)
  file(READ "${PRODUCTION_FILE}" PRODUCTION_CONTENT)
  string(FIND "${PRODUCTION_CONTENT}" "get_timing_arc_delay_map()" POSITION)
  if(NOT POSITION EQUAL -1)
    list(APPEND PRODUCTION_DELAY_MAP_FILES "${PRODUCTION_FILE}")
  endif()
endforeach()
set(EXPECTED_PRODUCTION_DELAY_MAP_FILES
    "${ISTA_SOURCE_ROOT}/source/module/sdf_writer/SDFWriter.cpp")
assert_same_inventory(PRODUCTION_DELAY_MAP_FILES EXPECTED_PRODUCTION_DELAY_MAP_FILES
                      "production timing-arc delay-map caller")
set(SDF_SOURCE "${ISTA_SOURCE_ROOT}/source/module/sdf_writer/SDFWriter.cpp")
require_present("${SDF_SOURCE}" "getSDFTimingArcDelay(const Arc& arc"
                "const SDF timing-arc reader")
require_present("${SDF_SOURCE}" "timing_arc_delay_map.find("
                "non-mutating SDF lookup")
require_absent("${SDF_SOURCE}" "timing_arc_delay_map[" "SDF timing-arc reader")
set(BASELINE_SOURCE "${CSR_TEST_ROOT}/baseline/StaFullBaselineProbe.cpp")
require_present("${BASELINE_SOURCE}" "const ista::Arc& arc ="
                "const baseline Arc reader")
file(GLOB_RECURSE TEST_CPP LIST_DIRECTORIES FALSE "${CSR_TEST_ROOT}/*.cpp")
set(TEST_DELAY_MAP_FILES)
foreach(TEST_FILE IN LISTS TEST_CPP)
  file(READ "${TEST_FILE}" TEST_CONTENT)
  string(FIND "${TEST_CONTENT}" "get_timing_arc_delay_map()" POSITION)
  if(NOT POSITION EQUAL -1)
    list(APPEND TEST_DELAY_MAP_FILES "${TEST_FILE}")
  endif()
endforeach()
set(EXPECTED_TEST_DELAY_MAP_FILES
    "${CSR_TEST_ROOT}/ArcTimingArcDelayTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonSupport.cpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleContractTest.cpp"
    "${BASELINE_SOURCE}")
assert_same_inventory(TEST_DELAY_MAP_FILES EXPECTED_TEST_DELAY_MAP_FILES
                      "test timing-arc delay-map caller")

message(STATUS "CSR adapter inventory, dependency, and direct-projector isolation passed")
