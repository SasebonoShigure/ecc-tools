# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

set(STEP8_EXPECTED_CANDIDATE_OWNER
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/real_differential/CsrRealDifferentialReferenceEncoding.cpp.o")
set(STEP8_EXPECTED_OPAQUE_OWNER
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/real_differential/StaCsrRealOracleProbe.cpp.o")
set(STEP8_EXPECTED_READER_OWNER
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/real_differential/CsrRealDifferentialOracleEncoding.cpp.o")
set(STEP8_EXPECTED_ORACLE_DTO_OBJECTS
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/real_differential/CsrRealDifferentialOracleEncoding.cpp.o"
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleReader.cpp.o"
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleNodeReader.cpp.o"
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleArcReader.cpp.o")
set(STEP8_ORACLE_CPP_SOURCES
    real_differential/StaCsrRealOracleProbe.cpp
    real_differential/CsrRealDifferentialOracleEncoding.cpp
    real_differential/CsrRealDifferentialWire.cpp
    real_differential/CsrRealDifferentialProbeSupport.cpp
    real_differential/CsrRealDifferentialScenario.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp)
set(STEP8_REFERENCE_CPP_SOURCES
    real_differential/StaCsrRealReferenceProbe.cpp
    real_differential/CsrRealDifferentialReferenceEncoding.cpp
    real_differential/CsrRealDifferentialNativeReadback.cpp
    real_differential/CsrRealDifferentialNativeNodeReadback.cpp
    real_differential/CsrRealDifferentialNativeArcReadback.cpp
    real_differential/CsrRealDifferentialWire.cpp
    real_differential/CsrRealDifferentialProbeSupport.cpp
    real_differential/CsrRealDifferentialScenario.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp)
set(STEP8_EXPECTED_PYTHON_SOURCES
    real_differential/real_differential_model.py
    real_differential/real_differential_wire_contract.py
    real_differential/real_differential_contract.py
    real_differential/real_differential_controller.py
    real_differential/real_differential_controller_selftest.py
    reference_input/reference_input_candidate_wire.py
    reference_input/reference_input_recompute_transfer.py)
set(STEP8_EXPECTED_STATIC_EDGES
    model>baseline_loader
    wire>model
    wire>baseline_contract
    contract>wire
    contract>reference_recompute
    controller>contract
    controller>selftest_lazy
    selftest>contract
    selftest>model
    selftest>wire
    selftest>candidate_codec
    selftest>reference_model_enum)
set(STEP8_LOCAL_PREFIX
    chipcompiler/thirdparty/ecc-tools/src/operation/iSTA/test)
set(STEP8_CI_PREFIX src/operation/iSTA/test)
set(STEP8_RUFF_COMMON
    --isolated --no-cache --line-length 100 --target-version py311)
set(STEP8_RUFF_CHECK_ONLY --select B,E,F,FBT,I,SIM,UP)
set(STEP8_LOCAL_CHECK .venv/bin/ruff check ${STEP8_RUFF_COMMON}
    ${STEP8_RUFF_CHECK_ONLY})
set(STEP8_LOCAL_FORMAT .venv/bin/ruff format --check ${STEP8_RUFF_COMMON})
set(STEP8_CI_CHECK uvx --from ruff==0.14.13 ruff check
    ${STEP8_RUFF_COMMON} ${STEP8_RUFF_CHECK_ONLY})
set(STEP8_CI_FORMAT uvx --from ruff==0.14.13 ruff format --check
    ${STEP8_RUFF_COMMON})
foreach(SOURCE IN LISTS STEP8_EXPECTED_PYTHON_SOURCES)
  list(APPEND STEP8_LOCAL_CHECK "${STEP8_LOCAL_PREFIX}/${SOURCE}")
  list(APPEND STEP8_LOCAL_FORMAT "${STEP8_LOCAL_PREFIX}/${SOURCE}")
  list(APPEND STEP8_CI_CHECK "${STEP8_CI_PREFIX}/${SOURCE}")
  list(APPEND STEP8_CI_FORMAT "${STEP8_CI_PREFIX}/${SOURCE}")
endforeach()
set(STEP8_EXPECTED_RUFF_VERSION "ruff 0.14.13")
set(STEP8_EXPECTED_LOCAL_CWD
    "canonical-parent:pyproject.toml+chipcompiler/thirdparty/ecc-tools")

function(isolation_assert_exact ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  set(ACTUAL ${${ACTUAL_VARIABLE}})
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  list(LENGTH ACTUAL ACTUAL_COUNT)
  list(LENGTH EXPECTED EXPECTED_COUNT)
  list(REMOVE_DUPLICATES ACTUAL)
  list(REMOVE_DUPLICATES EXPECTED)
  list(LENGTH ACTUAL ACTUAL_UNIQUE)
  list(LENGTH EXPECTED EXPECTED_UNIQUE)
  list(SORT ACTUAL)
  list(SORT EXPECTED)
  if(NOT ACTUAL_COUNT EQUAL ACTUAL_UNIQUE
     OR NOT EXPECTED_COUNT EQUAL EXPECTED_UNIQUE
     OR NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "Step 8 isolation detector rejected ${DESCRIPTION}: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()

function(isolation_assert_ordered ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  set(ACTUAL ${${ACTUAL_VARIABLE}})
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  set(UNIQUE ${ACTUAL})
  list(LENGTH ACTUAL COUNT)
  list(REMOVE_DUPLICATES UNIQUE)
  list(LENGTH UNIQUE UNIQUE_COUNT)
  if(NOT COUNT EQUAL UNIQUE_COUNT OR NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "Step 8 isolation detector rejected ${DESCRIPTION}: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()
function(isolation_check_detector CANDIDATE_VARIABLE OPAQUE_VARIABLE
         READER_VARIABLE DOMAIN_COUNT PRODUCTION_COUNT)
  isolation_assert_exact(${CANDIDATE_VARIABLE}
                         STEP8_EXPECTED_CANDIDATE_OWNER
                         "candidate call owner")
  isolation_assert_exact(${OPAQUE_VARIABLE} STEP8_EXPECTED_OPAQUE_OWNER
                         "oracle opaque call owner")
  isolation_assert_exact(${READER_VARIABLE} STEP8_EXPECTED_READER_OWNER
                         "native reader call owner")
  if(NOT DOMAIN_COUNT EQUAL 1 OR NOT PRODUCTION_COUNT EQUAL 0)
    message(FATAL_ERROR
            "Step 8 isolation detector rejected candidate domains: test=${DOMAIN_COUNT} production=${PRODUCTION_COUNT}")
  endif()
endfunction()

function(isolation_check_static_detector SOURCES_VARIABLE EDGES_VARIABLE
         LOCAL_CHECK_VARIABLE LOCAL_FORMAT_VARIABLE CI_CHECK_VARIABLE
         CI_FORMAT_VARIABLE VERSION CWD)
  isolation_assert_ordered(${SOURCES_VARIABLE} STEP8_EXPECTED_PYTHON_SOURCES
                           "static Python source inventory")
  isolation_assert_ordered(${EDGES_VARIABLE} STEP8_EXPECTED_STATIC_EDGES
                           "static Python DAG")
  foreach(PAIR
          "${LOCAL_CHECK_VARIABLE}|STEP8_LOCAL_CHECK|local Ruff check argv"
          "${LOCAL_FORMAT_VARIABLE}|STEP8_LOCAL_FORMAT|local Ruff format argv"
          "${CI_CHECK_VARIABLE}|STEP8_CI_CHECK|CI Ruff check argv"
          "${CI_FORMAT_VARIABLE}|STEP8_CI_FORMAT|CI Ruff format argv")
    string(REPLACE "|" ";" FIELDS "${PAIR}")
    list(GET FIELDS 0 ACTUAL)
    list(GET FIELDS 1 EXPECTED)
    list(GET FIELDS 2 DESCRIPTION)
    isolation_assert_ordered(${ACTUAL} ${EXPECTED} "${DESCRIPTION}")
  endforeach()
  if(NOT VERSION STREQUAL STEP8_EXPECTED_RUFF_VERSION
     OR NOT CWD STREQUAL STEP8_EXPECTED_LOCAL_CWD)
    message(FATAL_ERROR
            "Step 8 isolation detector rejected Ruff version/cwd: ${VERSION} [${CWD}]")
  endif()
endfunction()

if(DEFINED CSR_STEP8_STATIC_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP8_STATIC_SOURCES_PACKED CSR_STEP8_STATIC_EDGES_PACKED
                   CSR_STEP8_LOCAL_CHECK_PACKED CSR_STEP8_LOCAL_FORMAT_PACKED
                   CSR_STEP8_CI_CHECK_PACKED CSR_STEP8_CI_FORMAT_PACKED
                   CSR_STEP8_RUFF_VERSION CSR_STEP8_LOCAL_CWD)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  foreach(NAME SOURCES EDGES LOCAL_CHECK LOCAL_FORMAT CI_CHECK CI_FORMAT)
    string(TOUPPER "${NAME}" UPPER)
    if(NAME STREQUAL SOURCES OR NAME STREQUAL EDGES)
      set(PREFIX CSR_STEP8_STATIC)
    else()
      set(PREFIX CSR_STEP8)
    endif()
    string(REPLACE "|" ";" "DETECTOR_${NAME}"
           "${${PREFIX}_${UPPER}_PACKED}")
  endforeach()
  isolation_check_static_detector(
      DETECTOR_SOURCES DETECTOR_EDGES DETECTOR_LOCAL_CHECK
      DETECTOR_LOCAL_FORMAT DETECTOR_CI_CHECK DETECTOR_CI_FORMAT
      "${CSR_STEP8_RUFF_VERSION}" "${CSR_STEP8_LOCAL_CWD}")
  return()
endif()

if(DEFINED CSR_STEP8_ISOLATION_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP8_CANDIDATE_OWNERS_PACKED
                   CSR_STEP8_OPAQUE_OWNERS_PACKED
                   CSR_STEP8_READER_OWNERS_PACKED
                   CSR_STEP8_TEST_DOMAIN_COUNT
                   CSR_STEP8_PRODUCTION_COUNT)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  string(REPLACE "|" ";" DETECTOR_CANDIDATE
         "${CSR_STEP8_CANDIDATE_OWNERS_PACKED}")
  string(REPLACE "|" ";" DETECTOR_OPAQUE
         "${CSR_STEP8_OPAQUE_OWNERS_PACKED}")
  string(REPLACE "|" ";" DETECTOR_READER
         "${CSR_STEP8_READER_OWNERS_PACKED}")
  isolation_check_detector(
      DETECTOR_CANDIDATE DETECTOR_OPAQUE DETECTOR_READER
      "${CSR_STEP8_TEST_DOMAIN_COUNT}" "${CSR_STEP8_PRODUCTION_COUNT}")
  return()
endif()

function(isolation_run_detector EXPECT_SUCCESS CANDIDATE OPAQUE READER
         DOMAIN_COUNT PRODUCTION_COUNT)
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -DCSR_STEP8_ISOLATION_DETECTOR_MODE=TRUE
              "-DCSR_STEP8_CANDIDATE_OWNERS_PACKED=${CANDIDATE}"
              "-DCSR_STEP8_OPAQUE_OWNERS_PACKED=${OPAQUE}"
              "-DCSR_STEP8_READER_OWNERS_PACKED=${READER}"
              "-DCSR_STEP8_TEST_DOMAIN_COUNT=${DOMAIN_COUNT}"
              "-DCSR_STEP8_PRODUCTION_COUNT=${PRODUCTION_COUNT}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 8 isolation safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 8 isolation detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 8 isolation poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
  message(STATUS "Step 8 isolation ownership receipt: domains=${DOMAIN_COUNT},production=${PRODUCTION_COUNT},exit=${RESULT}")
endfunction()

isolation_run_detector(
    TRUE "${STEP8_EXPECTED_CANDIDATE_OWNER}"
    "${STEP8_EXPECTED_OPAQUE_OWNER}" "${STEP8_EXPECTED_READER_OWNER}" 1 0)
isolation_run_detector(
    FALSE "${STEP8_EXPECTED_CANDIDATE_OWNER}|unexpected.cpp.o"
    "${STEP8_EXPECTED_OPAQUE_OWNER}" "${STEP8_EXPECTED_READER_OWNER}" 1 0)
isolation_run_detector(
    FALSE "${STEP8_EXPECTED_CANDIDATE_OWNER}"
    "${STEP8_EXPECTED_OPAQUE_OWNER}" "${STEP8_EXPECTED_READER_OWNER}" 0 0)
isolation_run_detector(
    FALSE "${STEP8_EXPECTED_CANDIDATE_OWNER}"
    "${STEP8_EXPECTED_OPAQUE_OWNER}" "${STEP8_EXPECTED_READER_OWNER}" 1 1)

function(isolation_run_static_detector EXPECT_SUCCESS MUTATION)
  set(SOURCES ${STEP8_EXPECTED_PYTHON_SOURCES})
  set(EDGES ${STEP8_EXPECTED_STATIC_EDGES})
  set(LOCAL_CHECK ${STEP8_LOCAL_CHECK})
  set(LOCAL_FORMAT ${STEP8_LOCAL_FORMAT})
  set(CI_CHECK ${STEP8_CI_CHECK})
  set(CI_FORMAT ${STEP8_CI_FORMAT})
  set(VERSION "${STEP8_EXPECTED_RUFF_VERSION}")
  set(CWD "${STEP8_EXPECTED_LOCAL_CWD}")
  if(MUTATION STREQUAL missing_source)
    list(REMOVE_ITEM SOURCES real_differential/real_differential_wire_contract.py)
  elseif(MUTATION STREQUAL reverse_edge)
    list(APPEND EDGES selftest>controller)
  elseif(MUTATION STREQUAL widen_reference_model)
    list(APPEND EDGES selftest>reference_model_widen)
  elseif(MUTATION STREQUAL extra_config)
    list(APPEND LOCAL_CHECK --extend-exclude generated)
  elseif(MUTATION STREQUAL reorder_ci)
    list(REVERSE CI_FORMAT)
  elseif(MUTATION STREQUAL bad_version)
    set(VERSION "ruff 0.14.14")
  elseif(MUTATION STREQUAL bad_cwd)
    set(CWD child-checkout)
  elseif(NOT MUTATION STREQUAL safe)
    message(FATAL_ERROR "unknown Step 8 static mutation: ${MUTATION}")
  endif()
  foreach(NAME SOURCES EDGES LOCAL_CHECK LOCAL_FORMAT CI_CHECK CI_FORMAT)
    string(JOIN "|" "${NAME}_PACKED" ${${NAME}})
  endforeach()
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -DCSR_STEP8_STATIC_DETECTOR_MODE=TRUE
              "-DCSR_STEP8_STATIC_SOURCES_PACKED=${SOURCES_PACKED}"
              "-DCSR_STEP8_STATIC_EDGES_PACKED=${EDGES_PACKED}"
              "-DCSR_STEP8_LOCAL_CHECK_PACKED=${LOCAL_CHECK_PACKED}"
              "-DCSR_STEP8_LOCAL_FORMAT_PACKED=${LOCAL_FORMAT_PACKED}"
              "-DCSR_STEP8_CI_CHECK_PACKED=${CI_CHECK_PACKED}"
              "-DCSR_STEP8_CI_FORMAT_PACKED=${CI_FORMAT_PACKED}"
              "-DCSR_STEP8_RUFF_VERSION=${VERSION}"
              "-DCSR_STEP8_LOCAL_CWD=${CWD}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 8 static safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 8 isolation detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 8 static poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
  message(STATUS "Step 8 isolation detector receipt: ${MUTATION},exit=${RESULT}")
endfunction()

isolation_run_static_detector(TRUE safe)
foreach(MUTATION missing_source reverse_edge widen_reference_model extra_config reorder_ci bad_version bad_cwd)
  isolation_run_static_detector(FALSE "${MUTATION}")
endforeach()

foreach(REQUIRED CSR_TEST_ROOT CSR_BUILD_DIR NINJA_EXECUTABLE NM_EXECUTABLE
                 CSR_GENERATOR PYTHON_EXECUTABLE STEP8_PYTHON_SOURCES_PACKED)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "${REQUIRED} is required")
  endif()
endforeach()
if(NOT DEFINED STEP8_ALL_OBJECTS)
  message(FATAL_ERROR "Step 8 target gate must run before isolation gate")
endif()

function(isolation_read OUTPUT FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 8 isolation file: ${FILE_PATH}")
  endif()
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()
foreach(REQUIRED PYTHON_EXECUTABLE STEP8_PYTHON_SOURCES_PACKED)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "${REQUIRED} is required")
  endif()
endforeach()
string(REPLACE "|" ";" STEP8_ACTUAL_PYTHON "${STEP8_PYTHON_SOURCES_PACKED}")
isolation_assert_ordered(STEP8_ACTUAL_PYTHON STEP8_EXPECTED_PYTHON_SOURCES
                         "configured Python sources")
list(LENGTH STEP8_ACTUAL_PYTHON STEP8_ACTUAL_PYTHON_COUNT)
if(NOT STEP8_ACTUAL_PYTHON_COUNT EQUAL 7)
  message(FATAL_ERROR
          "Step 8 isolation exact-seven Python inventory differs: ${STEP8_ACTUAL_PYTHON_COUNT}")
endif()
foreach(SOURCE IN LISTS STEP8_ACTUAL_PYTHON)
  isolation_read(PYTHON_CONTENT "${CSR_TEST_ROOT}/${SOURCE}")
  string(SUBSTRING "${PYTHON_CONTENT}" 0 500 PYTHON_HEADER)
  string(FIND "${PYTHON_HEADER}" "Mulan PSL v2" MULAN_POSITION)
  if(MULAN_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 8 isolation exact-seven Mulan header missing: ${SOURCE}")
  endif()
  foreach(FORBIDDEN "# noqa" "# ruff: noqa" "# fmt: off" "# fmt: skip"
                    "fmt:off" "fmt:skip" "extend-exclude" "per-file-ignores")
    string(FIND "${PYTHON_CONTENT}" "${FORBIDDEN}" FORBIDDEN_POSITION)
    if(NOT FORBIDDEN_POSITION EQUAL -1)
      message(FATAL_ERROR
              "Step 8 isolation exact-seven Python hygiene violation: ${SOURCE}: ${FORBIDDEN}")
    endif()
  endforeach()
  string(FIND "${PYTHON_CONTENT}" "\r" CR_POSITION)
  if(NOT CR_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 8 isolation exact-seven Python contains CR bytes: ${SOURCE}")
  endif()
  string(REGEX MATCHALL "\n" PYTHON_NEWLINES "${PYTHON_CONTENT}")
  list(LENGTH PYTHON_NEWLINES PYTHON_LINES)
  set(PYTHON_LIMIT 700)
  if(SOURCE MATCHES "real_differential_controller(_selftest)?[.]py$")
    set(PYTHON_LIMIT 550)
  endif()
  if(NOT PYTHON_LINES LESS PYTHON_LIMIT)
    message(STATUS
            "Step 8 isolation exact-seven advisory line target exceeded: ${SOURCE}=${PYTHON_LINES}/${PYTHON_LIMIT}")
  endif()
endforeach()

function(isolation_count CONTENT_VARIABLE TOKEN OUTPUT)
  set(REMAINDER "${${CONTENT_VARIABLE}}")
  set(COUNT 0)
  string(LENGTH "${TOKEN}" TOKEN_LENGTH)
  while(TRUE)
    string(FIND "${REMAINDER}" "${TOKEN}" POSITION)
    if(POSITION EQUAL -1)
      break()
    endif()
    math(EXPR OFFSET "${POSITION} + ${TOKEN_LENGTH}")
    string(SUBSTRING "${REMAINDER}" ${OFFSET} -1 REMAINDER)
    math(EXPR COUNT "${COUNT} + 1")
  endwhile()
  set("${OUTPUT}" "${COUNT}" PARENT_SCOPE)
endfunction()

function(isolation_require_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
  isolation_count(${CONTENT_VARIABLE} "${TOKEN}" ACTUAL)
  if(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 8 isolation ${DESCRIPTION} count differs: ${ACTUAL}/${EXPECTED}")
  endif()
endfunction()

function(isolation_object TARGET SOURCE OUTPUT)
  set("${OUTPUT}"
      "src/operation/iSTA/test/CMakeFiles/${TARGET}.dir/${SOURCE}.o"
      PARENT_SCOPE)
endfunction()

function(isolation_deps OBJECT OUTPUT)
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${OBJECT}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE DEPS ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0 OR NOT DEPS MATCHES "[(]VALID[)]")
    message(FATAL_ERROR "missing/stale Step 8 isolation depfile: ${OBJECT}: ${ERROR}")
  endif()
  set("${OUTPUT}" "${DEPS}" PARENT_SCOPE)
endfunction()

function(isolation_undefined OBJECT OUTPUT)
  execute_process(
      COMMAND "${NM_EXECUTABLE}" -uC "${CSR_BUILD_DIR}/${OBJECT}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE SYMBOLS ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "cannot inspect Step 8 object symbols: ${OBJECT}: ${ERROR}")
  endif()
  set("${OUTPUT}" "${SYMBOLS}" PARENT_SCOPE)
endfunction()

set(ACTUAL_CANDIDATE_OWNERS)
set(ACTUAL_OPAQUE_OWNERS)
set(ACTUAL_READER_OWNERS)
foreach(OBJECT IN LISTS STEP8_ALL_OBJECTS)
  isolation_undefined("${OBJECT}" UNDEFINED)
  string(FIND "${UNDEFINED}" "canonicalIndependentReferenceInputBytes" CANDIDATE)
  string(FIND "${UNDEFINED}" "readAndEncodeNativeTiming" OPAQUE)
  string(FIND "${UNDEFINED}" "readNativeTimingOracle" READER)
  if(NOT CANDIDATE EQUAL -1)
    list(APPEND ACTUAL_CANDIDATE_OWNERS "${OBJECT}")
  endif()
  if(NOT OPAQUE EQUAL -1)
    list(APPEND ACTUAL_OPAQUE_OWNERS "${OBJECT}")
  endif()
  if(NOT READER EQUAL -1)
    list(APPEND ACTUAL_READER_OWNERS "${OBJECT}")
  endif()
endforeach()

set(ACTUAL_ORACLE_DTO_OBJECTS)
foreach(SOURCE IN LISTS STEP8_ORACLE_CPP_SOURCES)
  isolation_object(ista_csr_real_oracle_probe "${SOURCE}" OBJECT)
  isolation_deps("${OBJECT}" DEPS)
  string(FIND "${DEPS}" "CsrNativeTimingOracle" ORACLE_DTO)
  if(NOT ORACLE_DTO EQUAL -1)
    list(APPEND ACTUAL_ORACLE_DTO_OBJECTS "${OBJECT}")
  endif()
  if(NOT SOURCE MATCHES "CsrRealDifferentialOracleEncoding|CsrNativeTimingOracle")
    foreach(FORBIDDEN CsrCpuReferenceBackend buildCsrSessionSnapshot
                      computeCsrSessionReference scatterCsrSessionTimingResult
                      canonicalIndependentReferenceInputBytes
                      CsrReferencePoisonSupport)
      string(FIND "${DEPS}" "${FORBIDDEN}" FORBIDDEN_DEP)
      if(NOT FORBIDDEN_DEP EQUAL -1)
        message(FATAL_ERROR
                "oracle object has forbidden reference dependency: ${OBJECT}: ${FORBIDDEN}")
      endif()
    endforeach()
  endif()
endforeach()
isolation_assert_exact(ACTUAL_ORACLE_DTO_OBJECTS
                       STEP8_EXPECTED_ORACLE_DTO_OBJECTS
                       "oracle DTO dependency owners")

foreach(SOURCE IN LISTS STEP8_REFERENCE_CPP_SOURCES)
  isolation_object(ista_csr_real_reference_probe "${SOURCE}" OBJECT)
  isolation_deps("${OBJECT}" DEPS)
  string(FIND "${DEPS}" "CsrNativeTimingOracle" ORACLE_DEP)
  if(NOT ORACLE_DEP EQUAL -1)
    message(FATAL_ERROR "reference object depends on native oracle: ${OBJECT}")
  endif()
endforeach()

set(ENCODER_SOURCES
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp)
foreach(SOURCE IN LISTS ENCODER_SOURCES)
  isolation_object(ista_csr_real_reference_probe "${SOURCE}" OBJECT)
  isolation_deps("${OBJECT}" DEPS)
  foreach(FORBIDDEN CsrReferenceInputRawProjection Database.hpp
                    CsrNativeTimingOracle CsrReferenceCanonicalEncoder)
    string(FIND "${DEPS}" "${FORBIDDEN}" FORBIDDEN_DEP)
    if(NOT FORBIDDEN_DEP EQUAL -1)
      message(FATAL_ERROR
              "candidate encoder dependency escaped test-wire boundary: ${OBJECT}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

isolation_object(
    ista_csr_real_reference_probe
    real_differential/CsrRealDifferentialReferenceEncoding.cpp
    REFERENCE_ENCODING_OBJECT)
isolation_deps("${REFERENCE_ENCODING_OBJECT}" REFERENCE_ENCODING_DEPS)
foreach(FORBIDDEN Database.hpp CsrRealDifferentialNativeReadback.hpp)
  string(FIND "${REFERENCE_ENCODING_DEPS}" "${FORBIDDEN}" FORBIDDEN_DEP)
  if(NOT FORBIDDEN_DEP EQUAL -1)
    message(FATAL_ERROR
            "reference encoder dependency escaped detached DTO boundary: ${FORBIDDEN}")
  endif()
endforeach()

isolation_object(
    ista_csr_real_differential_wire_contract_test
    real_differential/CsrRealDifferentialWire.cpp WIRE_OBJECT)
isolation_deps("${WIRE_OBJECT}" WIRE_DEPS)
foreach(FORBIDDEN Database.hpp CsrNativeTimingOracle CsrCpuReferenceBackend
                  TimingResultScatter buildCsrSessionSnapshot)
  string(FIND "${WIRE_DEPS}" "${FORBIDDEN}" FORBIDDEN_DEP)
  if(NOT FORBIDDEN_DEP EQUAL -1)
    message(FATAL_ERROR "neutral wire dependency is not neutral: ${FORBIDDEN}")
  endif()
endforeach()

set(REFERENCE_ENCODING
    "${CSR_TEST_ROOT}/real_differential/CsrRealDifferentialReferenceEncoding.cpp")
set(INDEPENDENT_ENCODING
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentEncoding.cpp")
isolation_read(REFERENCE_ENCODING_CONTENT "${REFERENCE_ENCODING}")
isolation_read(INDEPENDENT_ENCODING_CONTENT "${INDEPENDENT_ENCODING}")
isolation_require_count(
    REFERENCE_ENCODING_CONTENT "canonicalIndependentReferenceInputBytes(input)" 1
    "independent candidate call")
isolation_require_count(
    INDEPENDENT_ENCODING_CONTENT
    "ista-csr-reference-input-test-wire/1.0.0" 1 "test-wire domain")
isolation_require_count(
    INDEPENDENT_ENCODING_CONTENT "appendSnapshotIdentity" 1
    "source identity encoder")
set(PRODUCTION_CALL_COUNT 0)
file(GLOB REAL_DIFFERENTIAL_CPP_HPP LIST_DIRECTORIES FALSE
     "${CSR_TEST_ROOT}/real_differential/*.cpp"
     "${CSR_TEST_ROOT}/real_differential/*.hpp")
foreach(FILE IN LISTS REAL_DIFFERENTIAL_CPP_HPP)
  isolation_read(FILE_CONTENT "${FILE}")
  isolation_count(FILE_CONTENT "canonicalReferenceInputBytes" FILE_PRODUCTION)
  math(EXPR PRODUCTION_CALL_COUNT
       "${PRODUCTION_CALL_COUNT} + ${FILE_PRODUCTION}")
  isolation_require_count(
      FILE_CONTENT "ista-csr-reference-input-test-wire/1.0.0" 0
      "copied test-wire domain in ${FILE}")
endforeach()

set(STEP8_AST_GATE [=[
import ast
import pathlib
import sys

names = ("model", "wire", "contract", "controller", "selftest")
paths = [pathlib.Path(value) for value in sys.argv[1:]]
texts = dict(zip(names, (path.read_text(encoding="utf-8") for path in paths), strict=True))
trees = {name: ast.parse(texts[name], filename=str(path)) for name, path in zip(names, paths, strict=True)}

def fail(message):
    raise SystemExit("Step 8 Python AST gate rejected " + message)

def imports(name):
    rows = []
    for node in ast.walk(trees[name]):
        if isinstance(node, ast.Import):
            rows.extend((alias.name, node.lineno) for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            rows.append((node.module, node.lineno))
    return rows

allowed = {"baseline_contract", "reference_input_bootstrap", "real_differential_model",
           "real_differential_wire_contract", "real_differential_contract"}
for name in names:
    for module, _line in imports(name):
        root = module.split(".")[0]
        if root not in sys.stdlib_module_names and module not in allowed and not module.startswith("reference_input_"):
            fail(f"third-party/unknown import {name}->{module}")

edges = set()
module_rows = {name: imports(name) for name in names}
if any(module in {"baseline_contract", "reference_input_bootstrap"} for module, _ in module_rows["model"]):
    edges.add("model>baseline_loader")
for module, _line in module_rows["wire"]:
    if module == "real_differential_model": edges.add("wire>model")
    if module == "baseline_contract": edges.add("wire>baseline_contract")
for module, _line in module_rows["contract"]:
    if module == "real_differential_wire_contract": edges.add("contract>wire")
    if module.startswith("reference_input_"): edges.add("contract>reference_recompute")
for module, _line in module_rows["controller"]:
    if module == "real_differential_contract": edges.add("controller>contract")
for module, _line in module_rows["selftest"]:
    if module == "real_differential_contract": edges.add("selftest>contract")
    if module == "real_differential_model": edges.add("selftest>model")
    if module == "real_differential_wire_contract": edges.add("selftest>wire")
    if module == "reference_input_candidate_wire": edges.add("selftest>candidate_codec")
    if module == "reference_input_model": edges.add("selftest>reference_model_enum")
    if module == "real_differential_controller": fail("selftest controller reverse edge")
self_reference = {module for module, _line in module_rows["selftest"] if module.startswith("reference_input_")}
if self_reference != {"reference_input_candidate_wire", "reference_input_model"}:
    fail(f"selftest reference imports {sorted(self_reference)!r}")

expected_dynamic = [
    ("controller", "_load_selftest_runner", "real_differential_controller_selftest")
]

def scan_dynamic(name, tree):
    rows = []

    class Visitor(ast.NodeVisitor):
        def __init__(self):
            self.owners = ["<module>"]

        def visit_FunctionDef(self, node):
            self.owners.append(node.name)
            self.generic_visit(node)
            self.owners.pop()

        visit_AsyncFunctionDef = visit_FunctionDef

        def visit_Call(self, node):
            target = node.func
            if isinstance(target, ast.Name) and target.id in {"__import__", "exec", "eval"}:
                fail(f"forbidden dynamic call {name}:{self.owners[-1]}:{target.id}")
            if isinstance(target, ast.Attribute) and target.attr == "import_module":
                if not (isinstance(target.value, ast.Name) and target.value.id == "importlib"):
                    fail(f"non-importlib import_module in {name}:{self.owners[-1]}")
                literal = (len(node.args) == 1 and not node.keywords and
                           isinstance(node.args[0], ast.Constant) and
                           isinstance(node.args[0].value, str))
                if not literal:
                    fail(f"nonliteral import_module in {name}:{self.owners[-1]}")
                rows.append((name, self.owners[-1], node.args[0].value))
            self.generic_visit(node)

        def visit_Attribute(self, node):
            if (isinstance(node.value, ast.Name) and node.value.id == "sys" and
                    node.attr == "modules"):
                fail(f"sys.modules lookup in {name}:{self.owners[-1]}")
            self.generic_visit(node)

    Visitor().visit(tree)
    return rows

def enforce_dynamic(rows):
    if rows != expected_dynamic:
        fail(f"dynamic selftest owner {rows!r}")

safe_dynamic = ast.parse('''import importlib
def _load_selftest_runner():
    return importlib.import_module("real_differential_controller_selftest")
''')
enforce_dynamic(scan_dynamic("controller", safe_dynamic))

def expect_dynamic_reject(label, source):
    try:
        enforce_dynamic(scan_dynamic("controller", ast.parse(source)))
    except SystemExit as error:
        if "Step 8 Python AST gate rejected" not in str(error):
            raise
    else:
        fail("dynamic scanner poison escaped " + label)

for label, source in (
    ("module-level", 'import importlib\nimportlib.import_module("real_differential_controller_selftest")'),
    ("nonliteral", 'import importlib\ndef _load_selftest_runner():\n return importlib.import_module(module_name)'),
    ("other-owner", 'import importlib\ndef other_owner():\n return importlib.import_module("real_differential_controller_selftest")'),
    ("duplicate", 'import importlib\ndef _load_selftest_runner():\n return (importlib.import_module("real_differential_controller_selftest"), importlib.import_module("real_differential_controller_selftest"))'),
    ("other-object", 'def _load_selftest_runner():\n return loader.import_module("real_differential_controller_selftest")'),
    ("builtin", 'def _load_selftest_runner():\n return __import__("real_differential_controller_selftest")'),
    ("sys-modules", 'import sys\ndef _load_selftest_runner():\n return sys.modules["real_differential_controller_selftest"]'),
):
    expect_dynamic_reject(label, source)

dynamic = []
for name in names:
    dynamic.extend(scan_dynamic(name, trees[name]))
enforce_dynamic(dynamic)

def mock_string_arg(node, index, keyword):
    if len(node.args) > index:
        value = node.args[index]
    else:
        values = [item.value for item in node.keywords if item.arg == keyword]
        value = values[0] if len(values) == 1 else None
    return value.value if isinstance(value, ast.Constant) and isinstance(value.value, str) else None

def popen_mock_calls(tree):
    rows = []
    string_patch = {"patch", "mock.patch", "unittest.mock.patch"}
    object_patch = {"patch.object", "mock.patch.object", "unittest.mock.patch.object"}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        callee = ast.unparse(node.func)
        if callee in string_patch and mock_string_arg(node, 0, "target") == "subprocess.Popen":
            rows.append((node.lineno, callee))
        if callee in object_patch:
            owner = ast.unparse(node.args[0]) if node.args else ""
            attribute = mock_string_arg(node, 1, "attribute")
            if owner == "subprocess" and attribute == "Popen":
                rows.append((node.lineno, callee))
    return rows

def enforce_no_popen_mock(name, tree):
    rows = popen_mock_calls(tree)
    if rows:
        fail(f"subprocess.Popen mocked in {name}: {rows!r}")

enforce_no_popen_mock("safe", ast.parse(
    'import secrets\nfrom unittest.mock import patch\npatch.object(secrets, "token_hex")'))

def expect_popen_mock_reject(label, source):
    try:
        enforce_no_popen_mock("poison", ast.parse(source))
    except SystemExit as error:
        if "Step 8 Python AST gate rejected" not in str(error):
            raise
    else:
        fail("Popen mock poison escaped " + label)

for label, source in (
    ("patch", 'patch("subprocess.Popen")'),
    ("mock-patch", 'mock.patch(target="subprocess.Popen")'),
    ("patch-object", 'patch.object(subprocess, "Popen")'),
    ("unittest-object", 'unittest.mock.patch.object(subprocess, attribute="Popen")'),
):
    expect_popen_mock_reject(label, source)
enforce_no_popen_mock("selftest", trees["selftest"])

edges.add("controller>selftest_lazy")
expected_edges = {"model>baseline_loader", "wire>model", "wire>baseline_contract",
                  "contract>wire", "contract>reference_recompute", "controller>contract",
                  "controller>selftest_lazy", "selftest>contract", "selftest>model", "selftest>wire",
                  "selftest>candidate_codec", "selftest>reference_model_enum"}
if edges != expected_edges: fail(f"DAG {sorted(edges)!r}")

def import_line(name, module):
    values = [line for imported, line in module_rows[name] if imported == module]
    return values[0] if len(values) == 1 else fail(f"{name} import {module} count={len(values)}")
def direct_from(name, module):
    rows = [[(alias.name, alias.asname) for alias in node.names] for node in trees[name].body
            if isinstance(node, ast.ImportFrom) and node.module == module]
    return rows[0] if len(rows) == 1 else fail(f"{name} from {module} count={len(rows)}")
if direct_from("selftest", "reference_input_candidate_wire") != [("decode_candidate_wire", None), ("encode_full_candidate_wire", None)]: fail("selftest codec imports")
if direct_from("selftest", "reference_input_model") != [("UnsupportedReason", None)]: fail("selftest enum import")
if import_line("wire", "real_differential_model") >= import_line("wire", "baseline_contract"):
    fail("wire bootstrap import order")
wire_line = import_line("contract", "real_differential_wire_contract")
if any(line <= wire_line for module, line in module_rows["contract"] if module.startswith("reference_input_")):
    fail("contract bootstrap import order")
self_model_line = import_line("selftest", "real_differential_model")
if any(import_line("selftest", module) <= self_model_line for module in ("reference_input_candidate_wire", "reference_input_model")):
    fail("selftest codec bootstrap import order")
if texts["selftest"].count("UnsupportedReason.PARASITIC_NET") != 1: fail("unsupported enum use")

path_owners = []
for name in names:
    for call in (node for node in ast.walk(trees[name]) if isinstance(node, ast.Call)):
        target = call.func
        if (isinstance(target, ast.Attribute) and target.attr == "insert" and
                isinstance(target.value, ast.Attribute) and target.value.attr == "path" and
                isinstance(target.value.value, ast.Name) and target.value.value.id == "sys"):
            path_owners.append(name)
if path_owners != ["model"]: fail(f"sys.path owners {path_owners!r}")
for token in ("_bootstrap_sibling_paths", '"baseline"', '"reference_input"', "sys.path.insert(0"):
    if token not in texts["model"]: fail("model bootstrap token " + token)
if "PROJECTION" in "".join(texts.values()): fail("projection bootstrap root")

error_owners = [name for name in names for node in trees[name].body
                if isinstance(node, ast.ClassDef) and node.name == "DifferentialContractError"]
if error_owners != ["wire"]: fail(f"DifferentialContractError owners {error_owners!r}")
for api in ("parse_strict_json_for_test", "validate_schema_for_test"):
    owners = [name for name in names for node in trees[name].body
              if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == api]
    if owners != ["wire"]: fail(f"{api} owners {owners!r}")

bundles = [(name, node) for name in names for node in trees[name].body
           if isinstance(node, ast.ClassDef) and node.name == "SelfTestApi"]
if len(bundles) != 1: fail(f"SelfTestApi owners {[name for name, _ in bundles]!r}")
fields = [node.target.id for node in bundles[0][1].body
          if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name)]
expected_fields = ["manifest_path", "baseline_manifest_path", "spawn_and_drain",
                   "prepare_layout", "commit_validated_run"]
if fields != expected_fields: fail(f"SelfTestApi fields {fields!r}")
decorators = bundles[0][1].decorator_list
if not any(isinstance(item, ast.Call) and any(key.arg == "frozen" and isinstance(key.value, ast.Constant)
           and key.value.value is True for key in item.keywords) for item in decorators):
    fail("SelfTestApi is not frozen")
self_runs = [node for node in trees["selftest"].body
             if isinstance(node, ast.FunctionDef) and node.name == "run_self_test"]
if len(self_runs) != 1 or [arg.arg for arg in self_runs[0].args.args] != ["api"]:
    fail("run_self_test signature")
if any(isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and
       node.value.id == "sys" and node.attr == "modules" for node in ast.walk(trees["selftest"])):
    fail("selftest sys.modules lookup")
if "add_mutually_exclusive_group" not in texts["controller"]:
    fail("mutually exclusive flags")
for flag in ("--self-test", "--public-negative-self-test"):
    if texts["controller"].count(flag) != 1: fail("flag owner " + flag)
if any(module == "reference_input_recompute_transfer" for module, _ in module_rows["contract"]):
    fail("contract direct transfer import")
]=])
set(STEP8_AST_FILES)
foreach(SOURCE IN LISTS STEP8_EXPECTED_PYTHON_SOURCES)
  if(SOURCE MATCHES "^real_differential/")
    list(APPEND STEP8_AST_FILES "${CSR_TEST_ROOT}/${SOURCE}")
  endif()
endforeach()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" -c "${STEP8_AST_GATE}" ${STEP8_AST_FILES}
                RESULT_VARIABLE AST_RESULT OUTPUT_VARIABLE AST_OUT ERROR_VARIABLE AST_ERR)
if(NOT AST_RESULT EQUAL 0)
  message(FATAL_ERROR "Step 8 Python AST contract failed: ${AST_OUT}${AST_ERR}")
endif()

get_filename_component(STEP8_CHILD_ROOT "${ISTA_SOURCE_ROOT}/../../.." ABSOLUTE)
set(STEP8_ACTION "${STEP8_CHILD_ROOT}/.github/actions/build-wheel/action.yml")
isolation_read(ACTION_CONTENT "${STEP8_ACTION}")
set(CI_CHECK_BLOCK [=[uvx --from ruff==0.14.13 ruff check \
          --isolated --no-cache --line-length 100 --target-version py311 \
          --select B,E,F,FBT,I,SIM,UP \
]=])
set(CI_FORMAT_BLOCK [=[uvx --from ruff==0.14.13 ruff format --check \
          --isolated --no-cache --line-length 100 --target-version py311 \
]=])
isolation_require_count(ACTION_CONTENT "${CI_CHECK_BLOCK}" 1 "CI Ruff check block")
isolation_require_count(ACTION_CONTENT "${CI_FORMAT_BLOCK}" 1 "CI Ruff format block")
foreach(SOURCE IN LISTS STEP8_EXPECTED_PYTHON_SOURCES)
  isolation_require_count(ACTION_CONTENT "${STEP8_CI_PREFIX}/${SOURCE}" 2
                          "CI Ruff source ${SOURCE}")
endforeach()
foreach(TOKEN "ista_csr_real_oracle_probe" "ista_csr_real_reference_probe"
              "ista_csr_real_differential_wire_contract_test"
              "-L '^ista_unit$'" "-L '^ista_property$'" "-L '^ista_smoke$'"
              "real_differential/__pycache__")
  isolation_require_count(ACTION_CONTENT "${TOKEN}" 1 "CI action token ${TOKEN}")
endforeach()
foreach(GATE CheckCsrStep8Targets.cmake CheckCsrStep8Workflow.cmake
             CsrRealDifferentialTests.cmake)
  isolation_read(GATE_CONTENT "${CSR_TEST_ROOT}/${GATE}")
  isolation_require_count(GATE_CONTENT "uvx --from ruff" 0
                          "CMake Ruff execution ${GATE}")
endforeach()

isolation_check_detector(
    ACTUAL_CANDIDATE_OWNERS ACTUAL_OPAQUE_OWNERS ACTUAL_READER_OWNERS
    1 "${PRODUCTION_CALL_COUNT}")
message(STATUS "CSR Step 8 TU, symbol, DTO, and candidate-wire isolation passed")
