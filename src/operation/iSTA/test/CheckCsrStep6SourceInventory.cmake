# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)
set(EXPECTED_NATIVE_ORACLE_SOURCES
    CsrNativeSemanticKeyTestSupport.hpp CsrNativeTimingOracleTestSupport.hpp CsrTimingScatterTestFixture.hpp CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp CsrNativeTimingOracleArcReader.cpp CsrNativeTimingOracleIndependentEncoding.cpp CsrTimingScatterTestFixture.cpp
    CsrReferenceInputTestFixture.cpp CsrReferenceInputNativeFixture.cpp CsrNativeGraphRawProjector.cpp CsrNativeTimingOracleContractTest.cpp)
set(EXPECTED_SCATTER_CONTRACT_SOURCES
    CsrNativeSemanticKeyTestSupport.hpp CsrNativeTimingOracleTestSupport.hpp CsrTimingScatterTestFixture.hpp CsrNativeTimingOracleReader.cpp CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp CsrNativeTimingOracleIndependentEncoding.cpp CsrTimingScatterTestFixture.cpp CsrReferenceInputTestFixture.cpp CsrReferenceInputNativeFixture.cpp
    CsrTimingResultFromNativeOracle.cpp CsrTimingScatterContractTest.cpp CsrTimingScatterStaleContract.cpp)
set(EXPECTED_SCATTER_ATOMICITY_SOURCES
    CsrNativeSemanticKeyTestSupport.hpp CsrNativeTimingOracleTestSupport.hpp CsrTimingScatterTestFixture.hpp CsrNativeTimingOracleReader.cpp CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp CsrNativeTimingOracleIndependentEncoding.cpp CsrTimingScatterTestFixture.cpp CsrReferenceInputTestFixture.cpp CsrReferenceInputNativeFixture.cpp
    CsrTimingResultFromNativeOracle.cpp CsrTimingScatterAtomicityContract.cpp)
set(EXPECTED_SCATTER_PROPERTY_SOURCES
    CsrNativeSemanticKeyTestSupport.hpp CsrNativeTimingOracleTestSupport.hpp CsrTimingScatterTestFixture.hpp CsrNativeTimingOracleReader.cpp CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp CsrNativeTimingOracleIndependentEncoding.cpp CsrTimingScatterTestFixture.cpp CsrReferenceInputTestFixture.cpp CsrReferenceInputNativeFixture.cpp
    CsrTimingResultFromNativeOracle.cpp CsrTimingScatterPropertyTest.cpp)
set(EXPECTED_PUBLIC_HEADER_SOURCES
    CsrTimingScatterPublicHeadersMain.cpp CsrTimingScatterHeaderIncludeOrderTest.cpp CsrTimingScatterHeaderReverseIncludeOrderTest.cpp
    public_header/CsrNativeTimingOracleHeaderTest.cpp public_header/TimingResultScatterHeaderTest.cpp)
set(EXPECTED_RERUN_SOURCES
    CsrNativeSemanticKeyTestSupport.hpp CsrNativeTimingOracleTestSupport.hpp CsrTimingScatterTestFixture.hpp CsrNativeTimingOracleReader.cpp CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp CsrNativeTimingOracleIndependentEncoding.cpp CsrTimingScatterTestFixture.cpp CsrReferenceInputTestFixture.cpp CsrReferenceInputNativeFixture.cpp
    CsrTimingResultFromNativeOracle.cpp timing_scatter/StaCsrTimingScatterRerunProbe.cpp)
set(EXPECTED_STEP8_ORACLE_OBJECTS
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/real_differential/CsrRealDifferentialOracleEncoding.cpp.o"
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleReader.cpp.o"
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleNodeReader.cpp.o"
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleArcReader.cpp.o")
set(EXPECTED_STEP8_SEMANTIC_OBJECTS
    "src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/CsrNativeTimingOracleReader.cpp.o")
function(require_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 6 inventory file: ${FILE_PATH}")
  endif()
endfunction()
function(read_required OUTPUT FILE_PATH)
  require_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()
function(require_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
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
function(assert_same_inventory ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
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
  if(NOT ACTUAL_COUNT EQUAL ACTUAL_UNIQUE OR NOT EXPECTED_COUNT EQUAL EXPECTED_UNIQUE
     OR NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "${DESCRIPTION} mismatch; actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()
if(DEFINED CSR_STEP6_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP6_DETECTOR_FILE CSR_STEP6_DETECTOR_TOKEN)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  if(CSR_STEP6_DETECTOR_MODE MATCHES "^(undefined|allocation)$")
    if(NOT DEFINED NM_EXECUTABLE)
      message(FATAL_ERROR "NM_EXECUTABLE is required")
    endif()
    execute_process(COMMAND "${NM_EXECUTABLE}" -uC "${CSR_STEP6_DETECTOR_FILE}"
                    RESULT_VARIABLE RESULT OUTPUT_VARIABLE CONTENT ERROR_VARIABLE ERROR)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 6 detector cannot inspect object: ${ERROR}")
    endif()
  else()
    read_required(CONTENT "${CSR_STEP6_DETECTOR_FILE}")
  endif()
  string(FIND "${CONTENT}" "${CSR_STEP6_DETECTOR_TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR
            "Step 6 ${CSR_STEP6_DETECTOR_MODE} detector rejected poison token: ${CSR_STEP6_DETECTOR_TOKEN}")
  endif()
  return()
endif()
if(DEFINED CSR_STEP6_PACKED_TEST_MODE)
  string(REPLACE "|" ";" CONTRACT "${CSR_SCATTER_CONTRACT_SOURCES_PACKED}")
  string(REPLACE "|" ";" PUBLIC "${CSR_PUBLIC_HEADER_SOURCES_PACKED}")
  string(REPLACE "|" ";" STEP8_ORACLE "${CSR_STEP8_ORACLE_OBJECTS_PACKED}")
  string(REPLACE "|" ";" STEP8_SEMANTIC "${CSR_STEP8_SEMANTIC_OBJECTS_PACKED}")
  set(EXPECTED_CONTRACT ${EXPECTED_SCATTER_CONTRACT_SOURCES})
  set(EXPECTED_PUBLIC ${EXPECTED_PUBLIC_HEADER_SOURCES})
  list(SORT CONTRACT)
  list(SORT PUBLIC)
  list(SORT EXPECTED_CONTRACT)
  list(SORT EXPECTED_PUBLIC)
  if(NOT "${CONTRACT}" STREQUAL "${EXPECTED_CONTRACT}"
     OR NOT "${PUBLIC}" STREQUAL "${EXPECTED_PUBLIC}")
    message(FATAL_ERROR "Step 6 packed detector mismatch: swapped contract/public lists")
  endif()
  assert_same_inventory(STEP8_ORACLE EXPECTED_STEP8_ORACLE_OBJECTS
                        "Step 8 oracle reverse detector")
  assert_same_inventory(STEP8_SEMANTIC EXPECTED_STEP8_SEMANTIC_OBJECTS
                        "Step 8 semantic reverse detector")
  return()
endif()
foreach(REQUIRED_VARIABLE ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR
        NINJA_EXECUTABLE CXX_COMPILER NM_EXECUTABLE CSR_GENERATOR
        CSR_NATIVE_ORACLE_SOURCES_PACKED CSR_SCATTER_CONTRACT_SOURCES_PACKED
        CSR_SCATTER_ATOMICITY_SOURCES_PACKED CSR_SCATTER_PROPERTY_SOURCES_PACKED
        CSR_PUBLIC_HEADER_SOURCES_PACKED CSR_RERUN_SOURCES_PACKED)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()
if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 6 inventory requires Ninja")
endif()
foreach(TOOL "${NINJA_EXECUTABLE}" "${CXX_COMPILER}" "${NM_EXECUTABLE}")
  require_file("${TOOL}")
endforeach()
set(ADAPTER_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr")
set(EXPECTED_PRODUCTION_FILES
    "${ADAPTER_DIR}/CsrNativeTimingOracle.hpp" "${ADAPTER_DIR}/TimingResultScatter.hpp"
    "${ADAPTER_DIR}/TimingResultScatterInternal.hpp" "${ADAPTER_DIR}/TimingResultScatter.cpp"
    "${ADAPTER_DIR}/TimingResultScatterValidation.cpp" "${ADAPTER_DIR}/TimingResultScatterPreparation.cpp"
    "${ADAPTER_DIR}/TimingResultScatterPublish.cpp")
set(EXPECTED_TEST_FILES
    "${CSR_TEST_ROOT}/CsrNativeSemanticKeyTestSupport.hpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleTestSupport.hpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleReader.cpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleNodeReader.cpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleArcReader.cpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleIndependentEncoding.cpp"
    "${CSR_TEST_ROOT}/CsrNativeTimingOracleContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterTestFixture.hpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterTestFixture.cpp"
    "${CSR_TEST_ROOT}/CsrTimingResultFromNativeOracle.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterStaleContract.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterAtomicityContract.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterPropertyTest.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterPublicHeadersMain.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterHeaderIncludeOrderTest.cpp"
    "${CSR_TEST_ROOT}/CsrTimingScatterHeaderReverseIncludeOrderTest.cpp"
    "${CSR_TEST_ROOT}/public_header/CsrNativeTimingOracleHeaderTest.cpp"
    "${CSR_TEST_ROOT}/public_header/TimingResultScatterHeaderTest.cpp"
    "${CSR_TEST_ROOT}/timing_scatter/StaCsrTimingScatterRerunProbe.cpp"
    "${CSR_TEST_ROOT}/timing_scatter/timing_scatter_controller.py"
    "${CSR_TEST_ROOT}/CsrTimingScatterTests.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep6SourceInventory.cmake")
list(LENGTH EXPECTED_TEST_FILES TEST_FILE_COUNT)
if(NOT TEST_FILE_COUNT EQUAL 23)
  message(FATAL_ERROR "Step 6 new test inventory must contain exactly 23 files")
endif()
file(GLOB ACTUAL_PRODUCTION_FILES LIST_DIRECTORIES FALSE
     "${ADAPTER_DIR}/CsrNativeTimingOracle*" "${ADAPTER_DIR}/TimingResultScatter*")
file(GLOB ACTUAL_TEST_FILES LIST_DIRECTORIES FALSE
     "${CSR_TEST_ROOT}/CsrNativeSemanticKeyTestSupport.hpp" "${CSR_TEST_ROOT}/CsrNativeTimingOracle*"
     "${CSR_TEST_ROOT}/CsrTimingScatter*" "${CSR_TEST_ROOT}/CsrTimingResultFromNativeOracle.cpp"
     "${CSR_TEST_ROOT}/public_header/CsrNativeTimingOracleHeaderTest.cpp"
     "${CSR_TEST_ROOT}/public_header/TimingResultScatterHeaderTest.cpp"
     "${CSR_TEST_ROOT}/timing_scatter/*" "${CSR_TEST_ROOT}/CheckCsrStep6SourceInventory.cmake")
assert_same_inventory(ACTUAL_PRODUCTION_FILES EXPECTED_PRODUCTION_FILES "Step 6 production files")
assert_same_inventory(ACTUAL_TEST_FILES EXPECTED_TEST_FILES "Step 6 test files")
string(CONCAT FORBIDDEN_CAST "const" "_cast")
foreach(STEP6_FILE IN LISTS EXPECTED_PRODUCTION_FILES EXPECTED_TEST_FILES)
  require_file("${STEP6_FILE}")
  file(STRINGS "${STEP6_FILE}" LINES)
  list(LENGTH LINES LINE_COUNT)
  if(LINE_COUNT GREATER_EQUAL 700)
    message(STATUS "Step 6 advisory line target reached or exceeded: ${STEP6_FILE}: ${LINE_COUNT}")
  endif()
  require_absent("${STEP6_FILE}" "${FORBIDDEN_CAST}" "Step 6 source")
  if(STEP6_FILE MATCHES "\\.(cpp|hpp)$")
    file(STRINGS "${STEP6_FILE}" CPP_INCLUDE_LINES
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]+\\.cpp[>\"]")
    if(CPP_INCLUDE_LINES)
      message(FATAL_ERROR "Step 6 source must not include a .cpp file: ${STEP6_FILE}")
    endif()
    file(STRINGS "${STEP6_FILE}" C_STYLE_CAST_LINES
         REGEX "\\((bool|char|double|float|int|long|short|size_t|std::size_t|[A-Z][A-Za-z0-9_:<>]*|[A-Za-z_][A-Za-z0-9_]*_t)[ *&]*\\)[ \t]*[A-Za-z0-9_(]")
    if(C_STYLE_CAST_LINES)
      message(FATAL_ERROR "Step 6 source contains a prohibited C-style cast: ${STEP6_FILE}")
    endif()
  endif()
endforeach()
function(run_detector MODE FILE_PATH TOKEN EXPECT_SUCCESS)
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DCSR_STEP6_DETECTOR_MODE=${MODE}"
                  "-DCSR_STEP6_DETECTOR_FILE=${FILE_PATH}"
                  "-DCSR_STEP6_DETECTOR_TOKEN=${TOKEN}" "-DNM_EXECUTABLE=${NM_EXECUTABLE}"
                  -P "${CMAKE_CURRENT_LIST_FILE}"
                  RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 6 ${MODE} safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "detector rejected poison token" DIAGNOSTIC)
    string(FIND "${ERR}" "${TOKEN}" TOKEN_DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1 OR TOKEN_DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 6 ${MODE} poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()
set(DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step6-detector-selftest")
file(REMOVE_RECURSE "${DETECTOR_DIR}")
file(MAKE_DIRECTORY "${DETECTOR_DIR}")
file(WRITE "${DETECTOR_DIR}/safe.txt" "safe\n")
file(WRITE "${DETECTOR_DIR}/oracle.txt" "#include \"CsrNativeTimingOracle.hpp\"\n")
file(WRITE "${DETECTOR_DIR}/semantic.txt" "#include \"CsrNativeSemanticKeyTestSupport.hpp\"\n")
file(WRITE "${DETECTOR_DIR}/duplicate.txt" "void makeArcBase();\n")
file(WRITE "${DETECTOR_DIR}/production.txt" "void assignParallelOrdinals();\n")
file(WRITE "${DETECTOR_DIR}/safe.cpp" "void safeSymbol() {}\n")
file(WRITE "${DETECTOR_DIR}/undefined.cpp"
     "extern void NativeMutationPoison(); void poison() { NativeMutationPoison(); }\n")
file(WRITE "${DETECTOR_DIR}/allocation.cpp"
     "#include <new>\nvoid* poison() { return ::operator new(16); }\n")
foreach(FIXTURE safe undefined allocation)
  execute_process(COMMAND "${CXX_COMPILER}" -std=c++20 -c "${DETECTOR_DIR}/${FIXTURE}.cpp"
                  -o "${DETECTOR_DIR}/${FIXTURE}.o" RESULT_VARIABLE RESULT
                  OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "cannot compile Step 6 detector fixture: ${OUT}${ERR}")
  endif()
endforeach()
foreach(MODE oracle-source oracle-depfile semantic-source semantic-depfile)
  if(MODE MATCHES "^oracle")
    set(POISON "${DETECTOR_DIR}/oracle.txt")
    set(TOKEN CsrNativeTimingOracle.hpp)
  else()
    set(POISON "${DETECTOR_DIR}/semantic.txt")
    set(TOKEN CsrNativeSemanticKeyTestSupport.hpp)
  endif()
  run_detector("${MODE}" "${DETECTOR_DIR}/safe.txt" "${TOKEN}" TRUE)
  run_detector("${MODE}" "${POISON}" "${TOKEN}" FALSE)
endforeach()
run_detector(duplicate-builder "${DETECTOR_DIR}/safe.txt" "makeArcBase(" TRUE)
run_detector(duplicate-builder "${DETECTOR_DIR}/duplicate.txt" "makeArcBase(" FALSE)
run_detector(production-helper "${DETECTOR_DIR}/safe.txt" assignParallelOrdinals TRUE)
run_detector(production-helper "${DETECTOR_DIR}/production.txt" assignParallelOrdinals FALSE)
run_detector(undefined "${DETECTOR_DIR}/safe.o" NativeMutationPoison TRUE)
run_detector(undefined "${DETECTOR_DIR}/undefined.o" NativeMutationPoison FALSE)
run_detector(allocation "${DETECTOR_DIR}/safe.o" "operator new" TRUE)
run_detector(allocation "${DETECTOR_DIR}/allocation.o" "operator new" FALSE)
file(REMOVE_RECURSE "${DETECTOR_DIR}")
string(JOIN "|" CONTRACT_PACKED ${EXPECTED_SCATTER_CONTRACT_SOURCES})
string(JOIN "|" PUBLIC_PACKED ${EXPECTED_PUBLIC_HEADER_SOURCES})
string(JOIN "|" STEP8_ORACLE_PACKED ${EXPECTED_STEP8_ORACLE_OBJECTS})
string(JOIN "|" STEP8_SEMANTIC_PACKED ${EXPECTED_STEP8_SEMANTIC_OBJECTS})
function(run_packed_detector EXPECT_SUCCESS ORACLE_PACKED SEMANTIC_PACKED CONTRACT_ARG PUBLIC_ARG)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DCSR_STEP6_PACKED_TEST_MODE=TRUE
                  "-DCSR_SCATTER_CONTRACT_SOURCES_PACKED=${CONTRACT_ARG}" "-DCSR_PUBLIC_HEADER_SOURCES_PACKED=${PUBLIC_ARG}"
                  "-DCSR_STEP8_ORACLE_OBJECTS_PACKED=${ORACLE_PACKED}" "-DCSR_STEP8_SEMANTIC_OBJECTS_PACKED=${SEMANTIC_PACKED}"
                  -P "${CMAKE_CURRENT_LIST_FILE}" RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 6 packed safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "mismatch" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 6 packed poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()
run_packed_detector(TRUE "${STEP8_ORACLE_PACKED}" "${STEP8_SEMANTIC_PACKED}" "${CONTRACT_PACKED}" "${PUBLIC_PACKED}")
run_packed_detector(FALSE "${STEP8_ORACLE_PACKED}" "${STEP8_SEMANTIC_PACKED}" "${PUBLIC_PACKED}" "${CONTRACT_PACKED}")
run_packed_detector(FALSE "${STEP8_ORACLE_PACKED}|src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/real_differential/StaCsrRealOracleProbe.cpp.o" "${STEP8_SEMANTIC_PACKED}" "${CONTRACT_PACKED}" "${PUBLIC_PACKED}")
run_packed_detector(FALSE "${STEP8_ORACLE_PACKED}|src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/real_differential/CsrRealDifferentialReferenceEncoding.cpp.o" "${STEP8_SEMANTIC_PACKED}" "${CONTRACT_PACKED}" "${PUBLIC_PACKED}")
run_packed_detector(FALSE "${STEP8_ORACLE_PACKED}" "${STEP8_SEMANTIC_PACKED}|src/operation/iSTA/test/CMakeFiles/ista_csr_real_oracle_probe.dir/real_differential/StaCsrRealOracleProbe.cpp.o" "${CONTRACT_PACKED}" "${PUBLIC_PACKED}")
run_packed_detector(FALSE "${STEP8_ORACLE_PACKED}" "${STEP8_SEMANTIC_PACKED}|src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/real_differential/CsrRealDifferentialReferenceEncoding.cpp.o" "${CONTRACT_PACKED}" "${PUBLIC_PACKED}")
set(BUILD_NINJA "${CSR_BUILD_DIR}/build.ninja")
require_file("${BUILD_NINJA}")
file(READ "${BUILD_NINJA}" BUILD_CONTENT)
execute_process(COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
                RESULT_VARIABLE RESULT OUTPUT_VARIABLE TARGET_OUTPUT ERROR_VARIABLE ERROR)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 6 Ninja targets: ${ERROR}")
endif()
string(REPLACE "\n" ";" TARGET_LINES "${TARGET_OUTPUT}")
function(extract_rule OBJECT OUTPUT)
  string(FIND "${BUILD_CONTENT}" "build ${OBJECT}:" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "missing Step 6 object rule: ${OBJECT}")
  endif()
  string(SUBSTRING "${BUILD_CONTENT}" ${POSITION} -1 TAIL)
  string(FIND "${TAIL}" "\n\n" END)
  string(SUBSTRING "${TAIL}" 0 ${END} RULE)
  set("${OUTPUT}" "${RULE}" PARENT_SCOPE)
endfunction()
function(check_deps OBJECT SOURCE OUTPUT)
  execute_process(COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${OBJECT}"
                  RESULT_VARIABLE RESULT OUTPUT_VARIABLE DEPS ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0 OR NOT DEPS MATCHES "\\(VALID\\)")
    message(FATAL_ERROR "missing valid Step 6 depfile: ${OBJECT}: ${ERROR}")
  endif()
  string(REPLACE "\n" ";" DEP_LINES "${DEPS}")
  set(CPP_DEPS)
  foreach(LINE IN LISTS DEP_LINES)
    string(STRIP "${LINE}" DEP)
    if(DEP MATCHES "\\.cpp$")
      cmake_path(NORMAL_PATH DEP)
      list(APPEND CPP_DEPS "${DEP}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES CPP_DEPS)
  cmake_path(NORMAL_PATH SOURCE)
  if(NOT "${CPP_DEPS}" STREQUAL "${SOURCE}")
    message(FATAL_ERROR "Step 6 depfile owner mismatch: ${OBJECT}: [${CPP_DEPS}]")
  endif()
  set("${OUTPUT}" "${DEPS}" PARENT_SCOPE)
endfunction()
function(check_target TARGET EXPECTED_VARIABLE PACKED_VARIABLE OBJECT_COUNT)
  string(REPLACE "|" ";" ACTUAL "${${PACKED_VARIABLE}}")
  assert_same_inventory(ACTUAL ${EXPECTED_VARIABLE} "${TARGET} source property")
  set(PREFIX "src/operation/iSTA/test/CMakeFiles/${TARGET}.dir/")
  set(EXPECTED_OBJECTS)
  foreach(SOURCE IN LISTS ${EXPECTED_VARIABLE})
    if(SOURCE MATCHES "\\.cpp$")
      set(OBJECT "${PREFIX}${SOURCE}.o")
      list(APPEND EXPECTED_OBJECTS "${OBJECT}")
      extract_rule("${OBJECT}" RULE)
      string(FIND "${RULE}" " ${CSR_TEST_ROOT}/${SOURCE} ||" OWNER)
      if(OWNER EQUAL -1)
        message(FATAL_ERROR "Step 6 object/source mismatch: ${OBJECT}")
      endif()
      foreach(OPTION -Wall -Wextra -Werror -pedantic -ffp-contract=off)
        string(FIND "${RULE}" "${OPTION}" OPTION_POSITION)
        if(OPTION_POSITION EQUAL -1)
          message(FATAL_ERROR "Step 6 object lacks ${OPTION}: ${OBJECT}")
        endif()
      endforeach()
      foreach(FAST -ffast-math -Ofast -ffp-contract=fast -fassociative-math)
        string(FIND "${RULE}" "${FAST}" FAST_POSITION)
        if(NOT FAST_POSITION EQUAL -1)
          message(FATAL_ERROR "Step 6 object uses ${FAST}: ${OBJECT}")
        endif()
      endforeach()
      check_deps("${OBJECT}" "${CSR_TEST_ROOT}/${SOURCE}" DEPS)
    endif()
  endforeach()
  list(LENGTH EXPECTED_OBJECTS COUNT)
  if(NOT COUNT EQUAL OBJECT_COUNT)
    message(FATAL_ERROR "${TARGET} object count must be ${OBJECT_COUNT}")
  endif()
  set(ACTUAL_OBJECTS)
  foreach(LINE IN LISTS TARGET_LINES)
    if(LINE MATCHES "^(${PREFIX}[^:]+\\.cpp\\.o):")
      list(APPEND ACTUAL_OBJECTS "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  assert_same_inventory(ACTUAL_OBJECTS EXPECTED_OBJECTS "${TARGET} Ninja objects")
  set(ALL_STEP6_OBJECTS ${ALL_STEP6_OBJECTS} ${EXPECTED_OBJECTS} PARENT_SCOPE)
endfunction()
set(ALL_STEP6_OBJECTS)
check_target(ista_csr_native_timing_oracle_contract_test EXPECTED_NATIVE_ORACLE_SOURCES CSR_NATIVE_ORACLE_SOURCES_PACKED 9)
check_target(ista_csr_timing_scatter_contract_test EXPECTED_SCATTER_CONTRACT_SOURCES CSR_SCATTER_CONTRACT_SOURCES_PACKED 10)
check_target(ista_csr_timing_scatter_atomicity_test EXPECTED_SCATTER_ATOMICITY_SOURCES CSR_SCATTER_ATOMICITY_SOURCES_PACKED 9)
check_target(ista_csr_timing_scatter_property_test EXPECTED_SCATTER_PROPERTY_SOURCES CSR_SCATTER_PROPERTY_SOURCES_PACKED 9)
check_target(ista_csr_timing_scatter_public_headers_test EXPECTED_PUBLIC_HEADER_SOURCES CSR_PUBLIC_HEADER_SOURCES_PACKED 5)
check_target(ista_csr_timing_scatter_rerun_probe EXPECTED_RERUN_SOURCES CSR_RERUN_SOURCES_PACKED 9)
set(EXPECTED_ORACLE_OBJECTS ${ALL_STEP6_OBJECTS})
set(PROVIDER_OBJECT_COUNT 0)
foreach(TARGET ista_csr_native_timing_oracle_contract_test ista_csr_timing_scatter_contract_test
               ista_csr_timing_scatter_atomicity_test ista_csr_timing_scatter_property_test ista_csr_timing_scatter_rerun_probe)
  foreach(PROVIDER CsrReferenceInputTestFixture.cpp CsrReferenceInputNativeFixture.cpp)
    set(OBJECT "src/operation/iSTA/test/CMakeFiles/${TARGET}.dir/${PROVIDER}.o")
    list(REMOVE_ITEM EXPECTED_ORACLE_OBJECTS "${OBJECT}")
    check_deps("${OBJECT}" "${CSR_TEST_ROOT}/${PROVIDER}" PROVIDER_DEPS)
    foreach(HEADER CsrNativeTimingOracle.hpp CsrNativeSemanticKeyTestSupport.hpp)
      require_count(PROVIDER_DEPS "${HEADER}" 0 "provider depfile ${OBJECT}")
    endforeach()
    math(EXPR PROVIDER_OBJECT_COUNT "${PROVIDER_OBJECT_COUNT} + 1")
  endforeach()
endforeach()
if(NOT PROVIDER_OBJECT_COUNT EQUAL 10)
  message(FATAL_ERROR "Step 6 fixture providers must own exactly 10 object instances")
endif()
list(REMOVE_ITEM EXPECTED_ORACLE_OBJECTS
     "src/operation/iSTA/test/CMakeFiles/ista_csr_timing_scatter_public_headers_test.dir/CsrTimingScatterPublicHeadersMain.cpp.o"
     "src/operation/iSTA/test/CMakeFiles/ista_csr_timing_scatter_public_headers_test.dir/public_header/TimingResultScatterHeaderTest.cpp.o" "src/operation/iSTA/test/CMakeFiles/ista_csr_native_timing_oracle_contract_test.dir/CsrNativeGraphRawProjector.cpp.o")
list(APPEND EXPECTED_ORACLE_OBJECTS ${EXPECTED_STEP8_ORACLE_OBJECTS})
set(TEST_CMAKE "${CSR_TEST_ROOT}/CsrTimingScatterTests.cmake")
read_required(TEST_CMAKE_CONTENT "${TEST_CMAKE}")
set(EXACT_OPTIONS [=[set(ISTA_STEP6_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)]=])
set(EXACT_LINKS [=[set(ISTA_STEP6_FUNCTIONAL_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)]=])
set(EXACT_RERUN_LINKS [=[set(ISTA_STEP6_RERUN_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_source
    idm
)]=])
require_count(TEST_CMAKE_CONTENT "${EXACT_OPTIONS}" 1 "Step 6 exact compile options")
require_count(TEST_CMAKE_CONTENT "${EXACT_LINKS}" 1 "Step 6 exact functional links")
require_count(TEST_CMAKE_CONTENT "${EXACT_RERUN_LINKS}" 1 "Step 6 exact rerun links")
require_count(TEST_CMAKE_CONTENT "target_link_libraries(" 1 "Step 6 direct-link helper")
foreach(TARGET ista_csr_native_timing_oracle_contract_test
               ista_csr_timing_scatter_contract_test ista_csr_timing_scatter_atomicity_test
               ista_csr_timing_scatter_property_test ista_csr_timing_scatter_public_headers_test
               ista_csr_timing_scatter_rerun_probe)
  require_present("${TEST_CMAKE}" "${TARGET}" "Step 6 executable")
endforeach()
read_required(NORMAL_HEADER_ORDER "${CSR_TEST_ROOT}/CsrTimingScatterHeaderIncludeOrderTest.cpp")
read_required(REVERSE_HEADER_ORDER "${CSR_TEST_ROOT}/CsrTimingScatterHeaderReverseIncludeOrderTest.cpp")
string(CONCAT STEP6_MULAN_PREFIX
    "// Copyright (c) 2023-2025 Peng Cheng Laboratory\n"
    "// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences\n"
    "// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip\n"
    "// Licensed under Mulan PSL v2.\n\n")
string(CONCAT EXPECTED_NORMAL_ORDER "${STEP6_MULAN_PREFIX}" "#include \"CsrNativeTimingOracle.hpp\"\n" "#include \"TimingResultScatter.hpp\"\n")
string(CONCAT EXPECTED_REVERSE_ORDER "${STEP6_MULAN_PREFIX}" "#include \"TimingResultScatter.hpp\"\n" "#include \"CsrNativeTimingOracle.hpp\"\n")
if(NOT NORMAL_HEADER_ORDER STREQUAL EXPECTED_NORMAL_ORDER OR NOT REVERSE_HEADER_ORDER STREQUAL EXPECTED_REVERSE_ORDER)
  message(FATAL_ERROR "Step 6 public-header normal/reverse include order mismatch")
endif()
set(MATERIALIZER "${CSR_TEST_ROOT}/CsrTimingResultFromNativeOracle.cpp")
foreach(TOKEN CsrCpuReferenceBackend TimingResultScatter DelayCalculator ClockPropagator
              TimingPropagator TimingAnalyzer STAInterface exportCsr replayFifo validateTimingResult)
  require_absent("${MATERIALIZER}" "${TOKEN}" "Step 6 materializer")
endforeach()
set(ORACLE_FILES CsrNativeTimingOracleReader.cpp CsrNativeTimingOracleNodeReader.cpp
                 CsrNativeTimingOracleArcReader.cpp CsrNativeTimingOracleIndependentEncoding.cpp)
foreach(ORACLE_FILE IN LISTS ORACLE_FILES)
  foreach(TOKEN CsrCpuReferenceBackend TimingResultScatter CsrTableLookup CsrFifoLevelization
                DelayCalculator ClockPropagator TimingPropagator TimingAnalyzer
                STAInterface exportCsr validateTimingResult replayFifo)
    require_absent("${CSR_TEST_ROOT}/${ORACLE_FILE}" "${TOKEN}" "native timing oracle")
  endforeach()
endforeach()
foreach(NO_HELPER CsrNativeTimingOracleNodeReader.cpp CsrNativeTimingOracleArcReader.cpp
                  CsrNativeTimingOracleIndependentEncoding.cpp)
  require_absent("${CSR_TEST_ROOT}/${NO_HELPER}" CsrNativeSemanticKeyTestSupport.hpp
                 "oracle subreader")
endforeach()
set(KEY_CONSUMERS CsrNativeGraphRawProjector.cpp CsrNativeGraphOracleReader.cpp
                  CsrNativeTimingOracleReader.cpp)
foreach(CONSUMER IN LISTS KEY_CONSUMERS)
  read_required(CONTENT "${CSR_TEST_ROOT}/${CONSUMER}")
  require_count(CONTENT CsrNativeSemanticKeyTestSupport.hpp 1
                "semantic helper include in ${CONSUMER}")
  foreach(DUPLICATE "makeNodeKey(" "makeArcBase(" "makeArcKeys(" "assignParallelOrdinals(")
    require_absent("${CSR_TEST_ROOT}/${CONSUMER}" "${DUPLICATE}" "semantic-key consumer")
  endforeach()
endforeach()
read_required(RAW "${CSR_TEST_ROOT}/CsrNativeGraphRawProjector.cpp")
read_required(GRAPH "${CSR_TEST_ROOT}/CsrNativeGraphOracleReader.cpp")
read_required(TIMING "${CSR_TEST_ROOT}/CsrNativeTimingOracleReader.cpp")
require_count(RAW "nativeSemanticNodeKey(" 4 "raw projector NodeKey calls")
require_count(RAW "nativeSemanticArcKeys(" 1 "raw projector ArcKey call")
require_count(GRAPH "nativeSemanticNodeKey(" 4 "graph oracle NodeKey calls")
require_count(TIMING "nativeSemanticNodeKey(" 1 "timing oracle NodeKey call")
require_count(TIMING "nativeSemanticArcKeys(" 1 "timing oracle ArcKey call")
read_required(CSR_TESTS_CONTENT "${CSR_TEST_ROOT}/CsrTests.cmake")
read_required(TOP_CMAKE_CONTENT "${CSR_TEST_ROOT}/CMakeLists.txt")
require_count(CSR_TESTS_CONTENT CsrNativeSemanticKeyTestSupport.hpp 1
              "legacy semantic helper source property")
require_count(TOP_CMAKE_CONTENT CsrNativeSemanticKeyTestSupport.hpp 1
              "projection semantic helper source property")
read_required(PATH_STATE "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingPathState.hpp")
foreach(GETTER get_start_point get_predecessor get_clock_name get_crpr_clock_pin)
  require_count(PATH_STATE "[[nodiscard]] const std::string& ${GETTER}() const noexcept" 1
                "const path-state getter ${GETTER}")
endforeach()
require_absent("${CSR_TEST_ROOT}/CsrNativeTimingOracleNodeReader.cpp"
               "TimingPathState native" "oracle path-state copy")
set(EXPECTED_SEMANTIC_OBJECTS)
foreach(TARGET ista_csr_graph_export_contract_test ista_csr_graph_encoding_contract_test
               ista_csr_graph_property_test ista_csr_projection_probe)
  foreach(SOURCE CsrNativeGraphRawProjector.cpp CsrNativeGraphOracleReader.cpp)
    list(APPEND EXPECTED_SEMANTIC_OBJECTS
         "src/operation/iSTA/test/CMakeFiles/${TARGET}.dir/${SOURCE}.o")
  endforeach()
endforeach()
foreach(TARGET ista_csr_native_timing_oracle_contract_test ista_csr_timing_scatter_contract_test
               ista_csr_timing_scatter_atomicity_test ista_csr_timing_scatter_property_test
               ista_csr_timing_scatter_rerun_probe)
  list(APPEND EXPECTED_SEMANTIC_OBJECTS
       "src/operation/iSTA/test/CMakeFiles/${TARGET}.dir/CsrNativeTimingOracleReader.cpp.o")
endforeach()
list(APPEND EXPECTED_SEMANTIC_OBJECTS "src/operation/iSTA/test/CMakeFiles/ista_csr_native_timing_oracle_contract_test.dir/CsrNativeGraphRawProjector.cpp.o")
list(APPEND EXPECTED_SEMANTIC_OBJECTS ${EXPECTED_STEP8_SEMANTIC_OBJECTS})
list(LENGTH EXPECTED_SEMANTIC_OBJECTS SEMANTIC_COUNT)
if(NOT SEMANTIC_COUNT EQUAL 15)
  message(FATAL_ERROR "semantic helper reverse allow-list must contain 15 objects")
endif()
find_program(AWK_EXECUTABLE awk REQUIRED)
set(AWK_PROGRAM [=[
/\.cpp\.o: #deps/ {
  owner=$0
  sub(/: #deps.*/, "", owner)
  valid=($0 ~ /\(VALID\)/ ? "VALID" : "STALE")
}
/CsrNativeTimingOracle.hpp/ { print "ORACLE|" owner "|" valid }
/CsrNativeSemanticKeyTestSupport.hpp/ { print "SEMANTIC|" owner "|" valid }
]=])
execute_process(COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps
                COMMAND "${AWK_EXECUTABLE}" "${AWK_PROGRAM}"
                RESULT_VARIABLE RESULT OUTPUT_VARIABLE REVERSE ERROR_VARIABLE ERROR)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "cannot scan Step 6 reverse dependencies: ${ERROR}")
endif()
string(REPLACE "\n" ";" ROWS "${REVERSE}")
set(ACTUAL_SEMANTIC_OBJECTS)
set(ACTUAL_ORACLE_OBJECTS)
foreach(ROW IN LISTS ROWS)
  if(ROW STREQUAL "")
    continue()
  endif()
  string(REPLACE "|" ";" FIELDS "${ROW}")
  list(GET FIELDS 0 KIND)
  list(GET FIELDS 1 OWNER)
  list(GET FIELDS 2 VALIDITY)
  if(NOT VALIDITY STREQUAL VALID)
    message(FATAL_ERROR "reverse dependency is stale: ${OWNER}")
  elseif(KIND STREQUAL SEMANTIC)
    list(APPEND ACTUAL_SEMANTIC_OBJECTS "${OWNER}")
  else()
    list(FIND EXPECTED_ORACLE_OBJECTS "${OWNER}" ALLOWED)
    if(ALLOWED EQUAL -1)
      message(FATAL_ERROR "oracle DTO reverse dependency escapes Step 6 objects: ${OWNER}")
    endif()
    list(APPEND ACTUAL_ORACLE_OBJECTS "${OWNER}")
  endif()
endforeach()
list(REMOVE_DUPLICATES ACTUAL_SEMANTIC_OBJECTS)
list(REMOVE_DUPLICATES ACTUAL_ORACLE_OBJECTS)
assert_same_inventory(ACTUAL_SEMANTIC_OBJECTS EXPECTED_SEMANTIC_OBJECTS
                      "semantic helper reverse dependencies")
assert_same_inventory(ACTUAL_ORACLE_OBJECTS EXPECTED_ORACLE_OBJECTS
                      "oracle DTO reverse dependencies")
foreach(OBJECT IN LISTS EXPECTED_SEMANTIC_OBJECTS)
  execute_process(COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${OBJECT}"
                  RESULT_VARIABLE RESULT OUTPUT_VARIABLE DEPS ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0 OR NOT DEPS MATCHES "\\(VALID\\)"
     OR NOT DEPS MATCHES "CsrNativeSemanticKeyTestSupport.hpp")
    message(FATAL_ERROR "semantic helper depfile missing/stale: ${OBJECT}: ${ERROR}")
  endif()
endforeach()
file(GLOB_RECURSE PRODUCTION_FILES LIST_DIRECTORIES FALSE
     "${ISTA_SOURCE_ROOT}/source/*.cpp" "${ISTA_SOURCE_ROOT}/source/*.hpp")
foreach(FILE IN LISTS PRODUCTION_FILES)
  file(STRINGS "${FILE}" INCLUDE
       REGEX "#[ \t]*include.*(CsrNativeTimingOracle|CsrNativeSemanticKeyTestSupport)")
  if(INCLUDE)
    message(FATAL_ERROR "test-only Step 6 header leaked into production: ${FILE}")
  endif()
endforeach()
set(PROBE "${CSR_TEST_ROOT}/timing_scatter/StaCsrTimingScatterRerunProbe.cpp")
read_required(PROBE_CONTENT "${PROBE}")
require_count(PROBE_CONTENT "DelayCalculator::getInst().init()" 1 "rerun DelayCalculator call")
require_count(PROBE_CONTENT "TimingPropagator::getInst().propagate()" 1 "rerun TimingPropagator call")
require_count(PROBE_CONTENT GraphBuilder 0 "rerun GraphBuilder call")
require_count(PROBE_CONTENT "installGuaranteedOverwriteCanaries(database, fixture.committed_input, baseline_oracle, options)" 1 "rerun canary install")
require_count(PROBE_CONTENT "guaranteed-overwrite canary did not change forward projection" 1 "rerun canary forward mutation")
require_count(PROBE_CONTENT "guaranteed-overwrite canary did not change dynamic Arc projection" 1 "rerun canary dynamic Arc mutation")
require_count(PROBE_CONTENT "pre-rerun forward projection unexpectedly equals baseline" 1 "rerun forward inequality")
require_count(PROBE_CONTENT "pre-rerun dynamic Arc projection unexpectedly equals baseline" 1 "rerun dynamic Arc inequality")
string(FIND "${PROBE_CONTENT}" "DelayCalculator::getInst().init()" DELAY)
string(FIND "${PROBE_CONTENT}" "TimingPropagator::getInst().propagate()" PROPAGATE)
if(PROPAGATE LESS DELAY)
  message(FATAL_ERROR "rerun call order must be DelayCalculator then TimingPropagator")
endif()
if(NOT DEFINED CMAKE_CTEST_COMMAND OR NOT EXISTS "${CMAKE_CTEST_COMMAND}")
  message(FATAL_ERROR "CMAKE_CTEST_COMMAND is required")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CSR_BUILD_DIR}"
                --show-only=json-v1 RESULT_VARIABLE RESULT
                OUTPUT_VARIABLE JSON ERROR_VARIABLE ERROR)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 6 CTests: ${ERROR}")
endif()
set(EXPECTED_CTESTS ista_csr_native_timing_oracle_contract_test
    ista_csr_timing_scatter_contract_test ista_csr_timing_scatter_atomicity_test
    ista_csr_timing_scatter_property_seed_1 ista_csr_timing_scatter_property_seed_7
    ista_csr_timing_scatter_property_seed_42
    ista_csr_timing_scatter_property_seed_20260820
    ista_csr_timing_scatter_property_seed_0xdeadbeef
    ista_csr_timing_scatter_public_headers_test ista_csr_timing_scatter_rerun
    ista_csr_step6_source_inventory)
string(JSON COUNT LENGTH "${JSON}" tests)
math(EXPR LAST "${COUNT} - 1")
set(ACTUAL_CTESTS)
foreach(INDEX RANGE 0 ${LAST})
  string(JSON NAME GET "${JSON}" tests ${INDEX} name)
  if(NOT NAME MATCHES "^ista_csr_(native_timing_oracle_contract_test|timing_scatter_|step6_source_inventory)")
    continue()
  endif()
  list(APPEND ACTUAL_CTESTS "${NAME}")
  string(JSON COMMAND_COUNT LENGTH "${JSON}" tests ${INDEX} command)
  set(COMMANDS)
  math(EXPR COMMAND_LAST "${COMMAND_COUNT} - 1")
  foreach(I RANGE 0 ${COMMAND_LAST})
    string(JSON VALUE GET "${JSON}" tests ${INDEX} command ${I})
    list(APPEND COMMANDS "${VALUE}")
  endforeach()
  set(LABEL)
  set(TIMEOUT)
  string(JSON PROPERTY_COUNT LENGTH "${JSON}" tests ${INDEX} properties)
  math(EXPR PROPERTY_LAST "${PROPERTY_COUNT} - 1")
  foreach(I RANGE 0 ${PROPERTY_LAST})
    string(JSON PROPERTY GET "${JSON}" tests ${INDEX} properties ${I} name)
    if(PROPERTY STREQUAL LABELS)
      string(JSON LABEL GET "${JSON}" tests ${INDEX} properties ${I} value 0)
    elseif(PROPERTY STREQUAL TIMEOUT)
      string(JSON TIMEOUT GET "${JSON}" tests ${INDEX} properties ${I} value)
    endif()
  endforeach()
  if(NAME MATCHES "_property_seed_(.+)$")
    set(EXPECTED_LABEL ista_property)
    set(EXPECTED_TIMEOUT 60)
    set(SEED "${CMAKE_MATCH_1}")
    if(NOT COMMAND_COUNT EQUAL 3
       OR NOT COMMANDS MATCHES "ista_csr_timing_scatter_property_test"
       OR NOT COMMANDS MATCHES "--seed;${SEED}$")
      message(FATAL_ERROR "property CTest command mismatch: ${NAME}: [${COMMANDS}]")
    endif()
  elseif(NAME STREQUAL ista_csr_timing_scatter_rerun)
    set(EXPECTED_LABEL ista_smoke)
    set(EXPECTED_TIMEOUT 300)
    string(JOIN "|" COMMAND_TEXT ${COMMANDS})
    require_count(COMMAND_TEXT timing_scatter_controller.py 1 "rerun controller")
    require_count(COMMAND_TEXT "--probe" 1 "rerun probe option")
    require_count(COMMAND_TEXT ista_csr_timing_scatter_rerun_probe 1 "rerun probe path")
  else()
    set(EXPECTED_LABEL ista_unit)
    set(EXPECTED_TIMEOUT 60)
    if(NAME STREQUAL ista_csr_step6_source_inventory)
      string(JOIN "|" COMMAND_TEXT ${COMMANDS})
      require_count(COMMAND_TEXT CheckCsrStep6SourceInventory.cmake 1 "inventory script")
    else()
      list(GET COMMANDS 0 COMMAND)
      get_filename_component(COMMAND_NAME "${COMMAND}" NAME)
      if(NOT COMMAND_COUNT EQUAL 1 OR NOT COMMAND_NAME STREQUAL NAME)
        message(FATAL_ERROR "unit CTest command mismatch: ${NAME}: [${COMMANDS}]")
      endif()
    endif()
  endif()
  if(NOT LABEL STREQUAL EXPECTED_LABEL OR NOT TIMEOUT EQUAL EXPECTED_TIMEOUT)
    message(FATAL_ERROR "CTest properties mismatch: ${NAME}: ${LABEL}/${TIMEOUT}")
  endif()
endforeach()
assert_same_inventory(ACTUAL_CTESTS EXPECTED_CTESTS "Step 6 exact 11 CTests")
list(LENGTH ACTUAL_CTESTS CTEST_COUNT)
if(NOT CTEST_COUNT EQUAL 11)
  message(FATAL_ERROR "Step 6 must register exactly 11 CTests")
endif()

file(GLOB_RECURSE ARTIFACTS LIST_DIRECTORIES TRUE
     "${ADAPTER_DIR}/CsrNativeTimingOracle*" "${ADAPTER_DIR}/TimingResultScatter*"
     "${CSR_TEST_ROOT}/CsrNativeTimingOracle*" "${CSR_TEST_ROOT}/CsrTimingScatter*"
     "${CSR_TEST_ROOT}/timing_scatter/*")
foreach(ARTIFACT IN LISTS ARTIFACTS)
  if(ARTIFACT MATCHES "(__pycache__|\\.pyc$|\\.o$|\\.d$|\\.patch$|\\.rej$|\\.orig$|\\.bak$|~$)")
    message(FATAL_ERROR "Step 6 source-tree artifact leakage: ${ARTIFACT}")
  endif()
endforeach()
message(STATUS "CSR Step 6 exact targets, CTests, reverse dependencies, and isolation passed")
