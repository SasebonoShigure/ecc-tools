# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)
if(POLICY CMP0130)
  cmake_policy(SET CMP0130 NEW)
endif()

function(file_contains_token FILE_PATH TOKEN RESULT_VARIABLE)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "token detector input is missing: ${FILE_PATH}")
  endif()
  file(READ "${FILE_PATH}" CONTENT)
  string(FIND "${CONTENT}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    set("${RESULT_VARIABLE}" FALSE PARENT_SCOPE)
  else()
    set("${RESULT_VARIABLE}" TRUE PARENT_SCOPE)
  endif()
endfunction()

function(require_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 4 isolation file: ${FILE_PATH}")
  endif()
endfunction()

function(require_absent FILE_PATH TOKEN DESCRIPTION)
  require_file("${FILE_PATH}")
  file_contains_token("${FILE_PATH}" "${TOKEN}" TOKEN_PRESENT)
  if(TOKEN_PRESENT)
    message(FATAL_ERROR "${DESCRIPTION} contains prohibited token ${TOKEN}: ${FILE_PATH}")
  endif()
endfunction()

if(DEFINED CSR_CONST_CAST_DETECTOR_SELFTEST_FILE)
  require_absent("${CSR_CONST_CAST_DETECTOR_SELFTEST_FILE}" "const_cast"
                 "const_cast detector self-test fixture")
  return()
endif()

foreach(REQUIRED_VARIABLE
        ISTA_SOURCE_ROOT
        CSR_TEST_ROOT
        CSR_STEP4_TARGET_SOURCE_ROWS_PACKED
        CSR_BUILD_DIR
        NINJA_EXECUTABLE
        CSR_GENERATOR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

set(REFERENCE_DIR "${CSR_TEST_ROOT}/reference_input")
set(PROJECTION_DIR "${CSR_TEST_ROOT}/projection")

function(require_hash FILE_PATH EXPECTED)
  require_file("${FILE_PATH}")
  file(SHA256 "${FILE_PATH}" ACTUAL)
  if(NOT ACTUAL STREQUAL EXPECTED)
    message(FATAL_ERROR "frozen projection file changed: ${FILE_PATH}: ${ACTUAL}")
  endif()
endfunction()

function(require_under_700 FILE_PATH)
  require_file("${FILE_PATH}")
  file(STRINGS "${FILE_PATH}" LINES)
  list(LENGTH LINES COUNT)
  if(COUNT GREATER_EQUAL 700)
    message(STATUS "Step 4 advisory line target reached or exceeded: ${FILE_PATH}: ${COUNT}")
  endif()
endfunction()

set(CONST_CAST_DETECTOR_SELFTEST_DIR
    "${CSR_BUILD_DIR}/csr-reference-input-const-cast-detector-selftest")
set(CONST_CAST_DETECTOR_POISON
    "${CONST_CAST_DETECTOR_SELFTEST_DIR}/contains-const-cast.hpp")
set(CONST_CAST_DETECTOR_SAFE
    "${CONST_CAST_DETECTOR_SELFTEST_DIR}/safe.hpp")
file(REMOVE_RECURSE "${CONST_CAST_DETECTOR_SELFTEST_DIR}")
file(MAKE_DIRECTORY "${CONST_CAST_DETECTOR_SELFTEST_DIR}")
file(WRITE "${CONST_CAST_DETECTOR_POISON}"
     "int &probe = const_cast<int &>(value);\n")
file(WRITE "${CONST_CAST_DETECTOR_SAFE}" "int probe = value;\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          "-DCSR_CONST_CAST_DETECTOR_SELFTEST_FILE=${CONST_CAST_DETECTOR_SAFE}"
          -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE CONST_CAST_DETECTOR_SAFE_RESULT
  OUTPUT_VARIABLE CONST_CAST_DETECTOR_SAFE_STDOUT
  ERROR_VARIABLE CONST_CAST_DETECTOR_SAFE_STDERR
)
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          "-DCSR_CONST_CAST_DETECTOR_SELFTEST_FILE=${CONST_CAST_DETECTOR_POISON}"
          -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE CONST_CAST_DETECTOR_POISON_RESULT
  OUTPUT_VARIABLE CONST_CAST_DETECTOR_POISON_STDOUT
  ERROR_VARIABLE CONST_CAST_DETECTOR_POISON_STDERR
)
string(FIND "${CONST_CAST_DETECTOR_POISON_STDERR}"
       "const_cast detector self-test fixture contains prohibited token const_cast:"
       CONST_CAST_DETECTOR_DIAGNOSTIC_POSITION)
file(REMOVE_RECURSE "${CONST_CAST_DETECTOR_SELFTEST_DIR}")
if(NOT CONST_CAST_DETECTOR_SAFE_RESULT EQUAL 0
   OR NOT CONST_CAST_DETECTOR_SAFE_STDOUT STREQUAL ""
   OR NOT CONST_CAST_DETECTOR_SAFE_STDERR STREQUAL "")
  message(FATAL_ERROR "const_cast detector safe-fixture self-test failed")
endif()
if(NOT CONST_CAST_DETECTOR_POISON_RESULT EQUAL 1
   OR NOT CONST_CAST_DETECTOR_POISON_STDOUT STREQUAL ""
   OR CONST_CAST_DETECTOR_DIAGNOSTIC_POSITION EQUAL -1)
  message(FATAL_ERROR "const_cast detector poison-fixture self-test failed")
endif()
if(EXISTS "${CONST_CAST_DETECTOR_SELFTEST_DIR}")
  message(FATAL_ERROR "const_cast detector build-local self-test cleanup failed")
endif()

require_hash("${PROJECTION_DIR}/projection_wire_fixture.py" "5d884ce2425771cb2d1ef77c1584e0107d4eb2d703f5d5963d7e4fadffbe7cf4")
require_hash("${PROJECTION_DIR}/projection_controller.py" "5ec34f6920da457823382ad15d1eb7e728cc06b6db80e15d470977db7e07c011")
require_hash("${PROJECTION_DIR}/projection_controller_selftest.py" "618252410083edb07f4f14f7307f6aeb85d52043280f19bd6cc5653d806e73ee")
require_hash("${PROJECTION_DIR}/StaCsrProjectionProbe.cpp" "ce5f3cdc6baf7bbd90ecbf62aab843191d4fde3d7e3b1f7037758a18e08a9bff")
require_hash("${PROJECTION_DIR}/projection_contract.py" "f91865827c4653134c2b73c557c5f1fef95252419558c28958954270a20b8acd")
require_hash("${PROJECTION_DIR}/projection_canonical.py" "f9aef86fdf8903a52449508f0e88d648d15b7096e074cf4eabaa83cd38f64acb")
set(DEFAULT_COMPAT "${PROJECTION_DIR}/projection_default_compat_v1.json")
require_hash("${DEFAULT_COMPAT}" "0da3a1b22ffd0ece8198c97854a78e9db342af42c5daad45ebfafb1c9f8fb414")
file(READ "${DEFAULT_COMPAT}" DEFAULT_COMPAT_CONTENT)
foreach(EXACT_COMPAT_TOKEN
        "ista-csr-projection-default-compat/1.0.0"
        "a91baa76ed378ba600621445d8ebe94f0b1636c56650da3f2a7e52f4f2ece64d"
        "aa90a070164f8561cadd68852ff34a88e4045c6b9a37902bab6304952028d9dd"
        "61a87d3fedefdcb12bc7c8868dbb361ad512f5f573413290d15fad08890bfa1f")
  string(FIND "${DEFAULT_COMPAT_CONTENT}" "${EXACT_COMPAT_TOKEN}" COMPAT_POSITION)
  if(COMPAT_POSITION EQUAL -1)
    message(FATAL_ERROR "default projection compat payload changed: ${EXACT_COMPAT_TOKEN}")
  endif()
endforeach()


set(EXPECTED_PYTHON_NAMES
    reference_input_bootstrap.py
    reference_input_candidate_wire.py
    reference_input_canary_wire.py
    reference_input_canonical.py
    reference_input_contract.py
    reference_input_controller.py
    reference_input_controller_negative_cases.py
    reference_input_controller_selftest.py
    reference_input_model.py
    reference_input_overlap_controller.py
    reference_input_raw_kat.py
    reference_input_raw_wire.py
    reference_input_recompute_fingerprint.py
    reference_input_recompute_graph.py
    reference_input_recompute_model.py
    reference_input_recompute_transfer.py
    reference_input_wire_fixture.py
)
file(GLOB PYTHON_FILES LIST_DIRECTORIES FALSE "${REFERENCE_DIR}/*.py")
set(ACTUAL_PYTHON_NAMES)
foreach(PYTHON_FILE IN LISTS PYTHON_FILES)
  get_filename_component(PYTHON_NAME "${PYTHON_FILE}" NAME)
  list(APPEND ACTUAL_PYTHON_NAMES "${PYTHON_NAME}")
endforeach()
list(SORT ACTUAL_PYTHON_NAMES)
list(SORT EXPECTED_PYTHON_NAMES)
if(NOT ACTUAL_PYTHON_NAMES STREQUAL EXPECTED_PYTHON_NAMES)
  message(FATAL_ERROR "Step 4 Python file inventory differs: ${ACTUAL_PYTHON_NAMES}")
endif()

set(EXPECTED_IMPORTS_reference_input_bootstrap "")
set(EXPECTED_IMPORTS_reference_input_model "")
set(EXPECTED_IMPORTS_reference_input_canonical "")
set(EXPECTED_IMPORTS_reference_input_candidate_wire reference_input_model reference_input_canonical)
set(EXPECTED_IMPORTS_reference_input_raw_wire reference_input_model reference_input_canonical)
set(EXPECTED_IMPORTS_reference_input_canary_wire reference_input_model reference_input_canonical)
set(EXPECTED_IMPORTS_reference_input_recompute_graph reference_input_model reference_input_canonical)
set(EXPECTED_IMPORTS_reference_input_recompute_transfer
    reference_input_model
    reference_input_canonical
    reference_input_recompute_graph
)
set(EXPECTED_IMPORTS_reference_input_recompute_model
    reference_input_model
    reference_input_canonical
    reference_input_recompute_graph
    reference_input_recompute_transfer
)
set(EXPECTED_IMPORTS_reference_input_recompute_fingerprint reference_input_model reference_input_canonical)
set(EXPECTED_IMPORTS_reference_input_raw_kat reference_input_model)
set(EXPECTED_IMPORTS_reference_input_overlap_controller "")
set(EXPECTED_IMPORTS_reference_input_contract
    reference_input_bootstrap
    reference_input_candidate_wire
    reference_input_raw_wire
    reference_input_canary_wire
    reference_input_recompute_graph
    reference_input_recompute_model
    reference_input_recompute_fingerprint
    reference_input_raw_kat
)
set(EXPECTED_IMPORTS_reference_input_controller reference_input_contract)
set(EXPECTED_IMPORTS_reference_input_wire_fixture reference_input_model reference_input_canonical)
set(EXPECTED_IMPORTS_reference_input_controller_negative_cases
    reference_input_contract
    reference_input_controller
    reference_input_wire_fixture
)
set(EXPECTED_IMPORTS_reference_input_controller_selftest
    reference_input_controller
    reference_input_controller_negative_cases
    reference_input_contract
    reference_input_recompute_transfer
    reference_input_wire_fixture
)
set(PYTHON_DAG_ORDER
    reference_input_bootstrap
    reference_input_model
    reference_input_canonical
    reference_input_overlap_controller
    reference_input_candidate_wire
    reference_input_raw_wire
    reference_input_canary_wire
    reference_input_recompute_graph
    reference_input_recompute_fingerprint
    reference_input_wire_fixture
    reference_input_raw_kat
    reference_input_recompute_transfer
    reference_input_recompute_model
    reference_input_contract
    reference_input_controller
    reference_input_controller_negative_cases
    reference_input_controller_selftest
)
foreach(PYTHON_FILE IN LISTS PYTHON_FILES)
  get_filename_component(PYTHON_STEM "${PYTHON_FILE}" NAME_WE)
  set(EXPECTED_IMPORT_VARIABLE "EXPECTED_IMPORTS_${PYTHON_STEM}")
  if(NOT DEFINED ${EXPECTED_IMPORT_VARIABLE})
    message(FATAL_ERROR "missing exact import-DAG row for ${PYTHON_STEM}")
  endif()
  file(STRINGS "${PYTHON_FILE}" LOCAL_IMPORT_LINES
       REGEX "^[ \t]*(from|import)[ \t]+reference_input_[a-z_]+")
  set(ACTUAL_LOCAL_IMPORTS)
  foreach(IMPORT_LINE IN LISTS LOCAL_IMPORT_LINES)
    if(IMPORT_LINE MATCHES "^[ \t]*from[ \t]+(reference_input_[a-z_]+)[ \t]+import")
      set(IMPORTED_MODULE "${CMAKE_MATCH_1}")
    elseif(IMPORT_LINE MATCHES "^[ \t]*import[ \t]+(reference_input_[a-z_]+)")
      set(IMPORTED_MODULE "${CMAKE_MATCH_1}")
    else()
      message(FATAL_ERROR "cannot parse local import edge: ${PYTHON_FILE}: ${IMPORT_LINE}")
    endif()
    list(FIND ACTUAL_LOCAL_IMPORTS "${IMPORTED_MODULE}" DUPLICATE_IMPORT_INDEX)
    if(NOT DUPLICATE_IMPORT_INDEX EQUAL -1)
      message(FATAL_ERROR "duplicate local import edge: ${PYTHON_STEM}->${IMPORTED_MODULE}")
    endif()
    list(APPEND ACTUAL_LOCAL_IMPORTS "${IMPORTED_MODULE}")
  endforeach()
  set(EXPECTED_LOCAL_IMPORTS ${${EXPECTED_IMPORT_VARIABLE}})
  list(SORT ACTUAL_LOCAL_IMPORTS)
  list(SORT EXPECTED_LOCAL_IMPORTS)
  if(NOT "${ACTUAL_LOCAL_IMPORTS}" STREQUAL "${EXPECTED_LOCAL_IMPORTS}")
    message(FATAL_ERROR
            "Step 4 import DAG differs for ${PYTHON_STEM}: actual=[${ACTUAL_LOCAL_IMPORTS}] expected=[${EXPECTED_LOCAL_IMPORTS}]")
  endif()
  list(FIND PYTHON_DAG_ORDER "${PYTHON_STEM}" IMPORTER_ORDER)
  foreach(IMPORTED_MODULE IN LISTS ACTUAL_LOCAL_IMPORTS)
    list(FIND PYTHON_DAG_ORDER "${IMPORTED_MODULE}" IMPORTED_ORDER)
    if(IMPORTED_ORDER EQUAL -1 OR IMPORTED_ORDER GREATER_EQUAL IMPORTER_ORDER)
      message(FATAL_ERROR "Step 4 import DAG is cyclic or unordered: ${PYTHON_STEM}->${IMPORTED_MODULE}")
    endif()
  endforeach()
endforeach()

set(EXPECTED_SCHEMA_NAMES
    reference_input_controller_v1.schema.json
    reference_input_result_v1.schema.json
)
file(GLOB SCHEMA_FILES LIST_DIRECTORIES FALSE "${REFERENCE_DIR}/schema/*")
set(ACTUAL_SCHEMA_NAMES)
foreach(SCHEMA_FILE IN LISTS SCHEMA_FILES)
  get_filename_component(SCHEMA_NAME "${SCHEMA_FILE}" NAME)
  list(APPEND ACTUAL_SCHEMA_NAMES "${SCHEMA_NAME}")
endforeach()
list(SORT ACTUAL_SCHEMA_NAMES)
list(SORT EXPECTED_SCHEMA_NAMES)
if(NOT ACTUAL_SCHEMA_NAMES STREQUAL EXPECTED_SCHEMA_NAMES)
  message(FATAL_ERROR "Step 4 schema inventory differs: ${ACTUAL_SCHEMA_NAMES}")
endif()

foreach(PYTHON_FILE IN LISTS PYTHON_FILES)
  require_under_700("${PYTHON_FILE}")
  require_absent("${PYTHON_FILE}" "from __future__" "Step 4 Python module")
  require_absent("${PYTHON_FILE}" "canonicalReferenceInputBytes" "Step 4 independent Python module")
  require_absent("${PYTHON_FILE}" "buildReferenceFingerprintPayloads" "Step 4 independent Python module")
endforeach()
foreach(SCHEMA_FILE IN LISTS SCHEMA_FILES)
  require_under_700("${SCHEMA_FILE}")
endforeach()

foreach(PYTHON_FILE IN LISTS PYTHON_FILES)
  get_filename_component(PYTHON_NAME "${PYTHON_FILE}" NAME)
  if(NOT PYTHON_NAME STREQUAL "reference_input_bootstrap.py")
    require_absent("${PYTHON_FILE}" "import projection_" "non-bootstrap Step 4 Python module")
    require_absent("${PYTHON_FILE}" "from projection_" "non-bootstrap Step 4 Python module")
  endif()
endforeach()

set(STEP4_CPP_FILES
    "${CSR_TEST_ROOT}/CsrReferencePublicHeadersMain.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceHeaderIncludeOrderTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjectionHeaderTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputTestFixture.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputNativeFixture.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputExportContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputFailureTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceTransferContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceSupportContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceFingerprintComponentsTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonIsolationTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputPropertyTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentEncoding.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentGraphEncoding.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentModelEncoding.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawConstraintProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawLibraryProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawParasiticProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonSupport.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputStorageOracle.cpp"
    "${CSR_TEST_ROOT}/CsrSnapshotStorageContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrSnapshotTestFixture.cpp"
    "${REFERENCE_DIR}/StaCsrReferenceInputProbe.cpp"
)
set(STEP4_HEADER_FILES
    "${CSR_TEST_ROOT}/CsrReferenceInputTestSupport.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentEncodingInternal.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjection.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjectionInternal.hpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonSupport.hpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonEncodingInternal.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputStorageOracle.hpp"
    "${CSR_TEST_ROOT}/CsrSnapshotTestFixture.hpp"
)
file(GLOB STEP4_PUBLIC_HEADER_FILES LIST_DIRECTORIES FALSE
     "${CSR_TEST_ROOT}/public_header/*.cpp")
set(STEP4_NATIVE_HEADER_FILES
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/Arc.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/Database.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/Instance.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/Net.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/ParasiticLibrary.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/ParasiticNet.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/ParasiticNode.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/ParasiticResistor.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/Pin.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingArc.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingCell.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingCellArc.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingCellPort.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingCheckArc.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingClock.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingConstraint.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingLibrary.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingPortConstraint.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingTable.hpp"
)
file(GLOB STEP4_PRODUCTION_FILES LIST_DIRECTORIES FALSE
     "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/*.cpp"
     "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/*.hpp"
     "${ISTA_SOURCE_ROOT}/source/module/timing_csr/*.cpp"
     "${ISTA_SOURCE_ROOT}/source/module/timing_csr/*.hpp"
     "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/*.cpp"
     "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/*.hpp")
foreach(STEP4_FILE IN LISTS STEP4_CPP_FILES STEP4_HEADER_FILES STEP4_PUBLIC_HEADER_FILES)
  require_under_700("${STEP4_FILE}")
endforeach()
foreach(STEP4_FILE
        IN LISTS STEP4_PRODUCTION_FILES
                 STEP4_NATIVE_HEADER_FILES
                 STEP4_CPP_FILES
                 STEP4_HEADER_FILES
                 STEP4_PUBLIC_HEADER_FILES)
  require_absent("${STEP4_FILE}" "const_cast" "Step 4 production/test inventory")
endforeach()

set(INDEPENDENT_ENCODERS
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentEncoding.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentGraphEncoding.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputIndependentModelEncoding.cpp"
)
foreach(ENCODER_FILE IN LISTS INDEPENDENT_ENCODERS)
  foreach(PROHIBITED_TOKEN
          CsrReferenceCanonicalEncoder
          CsrReferenceFingerprintPayload
          buildReferenceFingerprintPayloads
          makeReferenceInputIdentity
          canonicalReferenceInputBytes)
    require_absent("${ENCODER_FILE}" "${PROHIBITED_TOKEN}" "independent candidate encoder")
  endforeach()
endforeach()
set(STORAGE_ORACLE "${CSR_TEST_ROOT}/CsrReferenceInputStorageOracle.cpp")
foreach(PROHIBITED_TOKEN "snapshotStorage(" checkedStorage StorageWalker)
  require_absent("${STORAGE_ORACLE}" "${PROHIBITED_TOKEN}" "independent storage oracle")
endforeach()

set(RAW_FILES
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjection.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawConstraintProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawLibraryProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawParasiticProjector.cpp"
)
foreach(RAW_FILE IN LISTS RAW_FILES)
  foreach(PROHIBITED_TOKEN
          CsrReferenceTypes
          CsrReferenceCanonicalEncoder
          CsrReferenceFingerprintPayload
          buildReferenceFingerprintPayloads
          makeReferenceInputIdentity
          get_timing_point_map
          get_timing_order_list)
    require_absent("${RAW_FILE}" "${PROHIBITED_TOKEN}" "derived-free raw projector")
  endforeach()
endforeach()
require_absent("${CSR_TEST_ROOT}/CsrReferenceInputRawProjection.hpp" "Database.hpp" "DB-free raw DTO header")

set(EXPECTED_RAW_REFERENCE_FILES
    "${CSR_TEST_ROOT}/CsrReferenceInputExportContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputPropertyTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawConstraintProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawLibraryProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawParasiticProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjectionHeaderTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjectionInternal.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceInputRawProjector.cpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonIsolationTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferencePoisonSupport.hpp"
    "${CSR_TEST_ROOT}/CsrReferenceSupportContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrReferenceTransferContractTest.cpp"
    "${REFERENCE_DIR}/StaCsrReferenceInputProbe.cpp"
    "${CSR_TEST_ROOT}/real_differential/StaCsrRealReferenceProbe.cpp"
)
file(GLOB_RECURSE RAW_REFERENCE_CANDIDATES LIST_DIRECTORIES FALSE
     "${CSR_TEST_ROOT}/*.cpp"
     "${CSR_TEST_ROOT}/*.hpp")
set(ACTUAL_RAW_REFERENCE_FILES)
foreach(RAW_REFERENCE_CANDIDATE IN LISTS RAW_REFERENCE_CANDIDATES)
  file(READ "${RAW_REFERENCE_CANDIDATE}" RAW_REFERENCE_CONTENT)
  string(FIND "${RAW_REFERENCE_CONTENT}" "CsrReferenceInputRawProjection.hpp" RAW_REFERENCE_POSITION)
  if(NOT RAW_REFERENCE_POSITION EQUAL -1)
    list(APPEND ACTUAL_RAW_REFERENCE_FILES "${RAW_REFERENCE_CANDIDATE}")
  endif()
endforeach()
list(SORT ACTUAL_RAW_REFERENCE_FILES)
list(SORT EXPECTED_RAW_REFERENCE_FILES)
if(NOT "${ACTUAL_RAW_REFERENCE_FILES}" STREQUAL "${EXPECTED_RAW_REFERENCE_FILES}")
  message(FATAL_ERROR
          "raw DTO direct-reference inventory differs: actual=[${ACTUAL_RAW_REFERENCE_FILES}] expected=[${EXPECTED_RAW_REFERENCE_FILES}]")
endif()

get_filename_component(ECC_TOOLS_ROOT "${ISTA_SOURCE_ROOT}/../../.." ABSOLUTE)
file(GLOB_RECURSE STEP4_PROBE_SOURCES LIST_DIRECTORIES FALSE
     "${ECC_TOOLS_ROOT}/*StaCsrReferenceInputProbe.cpp")
set(EXPECTED_STEP4_PROBE_SOURCES "${REFERENCE_DIR}/StaCsrReferenceInputProbe.cpp")
if(NOT "${STEP4_PROBE_SOURCES}" STREQUAL "${EXPECTED_STEP4_PROBE_SOURCES}")
  message(FATAL_ERROR
          "Step 4 probe source must be globally unique: actual=[${STEP4_PROBE_SOURCES}] expected=[${EXPECTED_STEP4_PROBE_SOURCES}]")
endif()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 4 object isolation requires a Ninja generator: ${CSR_GENERATOR}")
endif()
set(BUILD_NINJA "${CSR_BUILD_DIR}/build.ninja")
require_file("${BUILD_NINJA}")
file(READ "${BUILD_NINJA}" BUILD_NINJA_CONTENT)
execute_process(
  COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
  RESULT_VARIABLE NINJA_TARGETS_RESULT
  OUTPUT_VARIABLE NINJA_TARGETS_OUTPUT
  ERROR_VARIABLE NINJA_TARGETS_ERROR
)
if(NOT NINJA_TARGETS_RESULT EQUAL 0)
  message(FATAL_ERROR
          "cannot inspect Step 4 Ninja targets: ${NINJA_TARGETS_ERROR}")
endif()
string(REPLACE "\n" ";" NINJA_TARGET_LINES "${NINJA_TARGETS_OUTPUT}")

set(EXPECTED_STEP4_TARGETS
    ista_csr_reference_public_headers_test
    ista_csr_reference_input_export_contract_test
    ista_csr_reference_input_failure_test
    ista_csr_reference_transfer_contract_test
    ista_csr_reference_support_contract_test
    ista_csr_reference_fingerprint_components_test
    ista_csr_reference_poison_isolation_test
    ista_csr_reference_input_property_test
    ista_csr_reference_input_probe
)
set(RAW_DEPENDENT_SOURCES
    CsrReferenceInputRawProjectionHeaderTest.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
    CsrReferenceInputExportContractTest.cpp
    CsrReferenceTransferContractTest.cpp
    CsrReferenceSupportContractTest.cpp
    CsrReferencePoisonIsolationTest.cpp
    CsrReferenceInputPropertyTest.cpp
    CsrReferencePoisonSupport.cpp
    reference_input/StaCsrReferenceInputProbe.cpp
)
string(REPLACE "|" ";" STEP4_TARGET_SOURCE_ROWS
       "${CSR_STEP4_TARGET_SOURCE_ROWS_PACKED}")
set(ACTUAL_STEP4_TARGETS)
set(EXPECTED_STEP4_OBJECTS)
set(EXPECTED_RAW_DEPENDENT_OBJECTS)
foreach(TARGET_SOURCE_ROW IN LISTS STEP4_TARGET_SOURCE_ROWS)
  string(FIND "${TARGET_SOURCE_ROW}" "=" TARGET_SOURCE_SEPARATOR)
  if(TARGET_SOURCE_SEPARATOR LESS 1)
    message(FATAL_ERROR "invalid Step 4 target/source row: ${TARGET_SOURCE_ROW}")
  endif()
  string(SUBSTRING "${TARGET_SOURCE_ROW}" 0 ${TARGET_SOURCE_SEPARATOR} STEP4_TARGET)
  math(EXPR TARGET_SOURCES_OFFSET "${TARGET_SOURCE_SEPARATOR} + 1")
  string(SUBSTRING "${TARGET_SOURCE_ROW}" ${TARGET_SOURCES_OFFSET} -1 STEP4_SOURCE_CSV)
  string(REPLACE "," ";" STEP4_SOURCES "${STEP4_SOURCE_CSV}")
  list(APPEND ACTUAL_STEP4_TARGETS "${STEP4_TARGET}")
  set(TARGET_OBJECT_PREFIX
      "src/operation/iSTA/test/CMakeFiles/${STEP4_TARGET}.dir/")
  set(EXPECTED_TARGET_OBJECTS)
  foreach(STEP4_SOURCE IN LISTS STEP4_SOURCES)
    set(STEP4_OBJECT "${TARGET_OBJECT_PREFIX}${STEP4_SOURCE}.o")
    set(STEP4_ABSOLUTE_SOURCE "${CSR_TEST_ROOT}/${STEP4_SOURCE}")
    list(APPEND EXPECTED_TARGET_OBJECTS "${STEP4_OBJECT}")
    list(APPEND EXPECTED_STEP4_OBJECTS "${STEP4_OBJECT}")
    list(FIND RAW_DEPENDENT_SOURCES "${STEP4_SOURCE}" RAW_DEPENDENT_INDEX)
    if(NOT RAW_DEPENDENT_INDEX EQUAL -1)
      list(APPEND EXPECTED_RAW_DEPENDENT_OBJECTS "${STEP4_OBJECT}")
    endif()

    set(OBJECT_RULE_TOKEN "build ${STEP4_OBJECT}:")
    string(FIND "${BUILD_NINJA_CONTENT}" "${OBJECT_RULE_TOKEN}" OBJECT_RULE_POSITION)
    if(OBJECT_RULE_POSITION EQUAL -1)
      message(FATAL_ERROR "missing Step 4 Ninja object rule: ${STEP4_OBJECT}")
    endif()
    string(SUBSTRING "${BUILD_NINJA_CONTENT}" ${OBJECT_RULE_POSITION} -1 OBJECT_RULE_TAIL)
    string(LENGTH "${OBJECT_RULE_TOKEN}" OBJECT_RULE_TOKEN_LENGTH)
    string(SUBSTRING "${OBJECT_RULE_TAIL}" ${OBJECT_RULE_TOKEN_LENGTH} -1 OBJECT_RULE_REMAINDER)
    string(FIND "${OBJECT_RULE_REMAINDER}" "${OBJECT_RULE_TOKEN}" DUPLICATE_RULE_POSITION)
    if(NOT DUPLICATE_RULE_POSITION EQUAL -1)
      message(FATAL_ERROR "duplicate Step 4 Ninja object rule: ${STEP4_OBJECT}")
    endif()
    string(FIND "${OBJECT_RULE_TAIL}" "\n\n" OBJECT_RULE_END)
    if(OBJECT_RULE_END EQUAL -1)
      message(FATAL_ERROR "unterminated Step 4 Ninja object rule: ${STEP4_OBJECT}")
    endif()
    string(SUBSTRING "${OBJECT_RULE_TAIL}" 0 ${OBJECT_RULE_END} OBJECT_RULE)
    string(FIND "${OBJECT_RULE}" " ${STEP4_ABSOLUTE_SOURCE} ||" SOURCE_RULE_POSITION)
    if(SOURCE_RULE_POSITION EQUAL -1)
      message(FATAL_ERROR
              "Step 4 Ninja source mapping differs: ${STEP4_OBJECT} -> ${STEP4_ABSOLUTE_SOURCE}")
    endif()
    string(FIND "${OBJECT_RULE}" "\n  DEP_FILE = ${STEP4_OBJECT}.d\n" DEP_FILE_POSITION)
    if(DEP_FILE_POSITION EQUAL -1)
      message(FATAL_ERROR "Step 4 Ninja depfile mapping differs: ${STEP4_OBJECT}")
    endif()

    execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${STEP4_OBJECT}"
      RESULT_VARIABLE OBJECT_DEPS_RESULT
      OUTPUT_VARIABLE OBJECT_DEPS_OUTPUT
      ERROR_VARIABLE OBJECT_DEPS_ERROR
    )
    if(NOT OBJECT_DEPS_RESULT EQUAL 0
       OR NOT OBJECT_DEPS_OUTPUT MATCHES "\\(VALID\\)"
       OR NOT OBJECT_DEPS_OUTPUT MATCHES "${STEP4_ABSOLUTE_SOURCE}")
      message(FATAL_ERROR
              "missing valid compiled depfile for ${STEP4_OBJECT}: ${OBJECT_DEPS_OUTPUT}${OBJECT_DEPS_ERROR}")
    endif()
    string(FIND "${OBJECT_DEPS_OUTPUT}"
           "${CSR_TEST_ROOT}/CsrReferenceInputRawProjection.hpp"
           RAW_DEPENDENCY_POSITION)
    if(RAW_DEPENDENT_INDEX EQUAL -1 AND NOT RAW_DEPENDENCY_POSITION EQUAL -1)
      message(FATAL_ERROR "unexpected raw DTO dependency: ${STEP4_OBJECT}")
    elseif(NOT RAW_DEPENDENT_INDEX EQUAL -1 AND RAW_DEPENDENCY_POSITION EQUAL -1)
      message(FATAL_ERROR "missing raw DTO dependency: ${STEP4_OBJECT}")
    endif()
  endforeach()

  set(ACTUAL_TARGET_OBJECTS)
  foreach(NINJA_TARGET_LINE IN LISTS NINJA_TARGET_LINES)
    if(NINJA_TARGET_LINE MATCHES "^(${TARGET_OBJECT_PREFIX}[^:]+\\.cpp\\.o):")
      list(APPEND ACTUAL_TARGET_OBJECTS "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  list(SORT ACTUAL_TARGET_OBJECTS)
  list(SORT EXPECTED_TARGET_OBJECTS)
  if(NOT "${ACTUAL_TARGET_OBJECTS}" STREQUAL "${EXPECTED_TARGET_OBJECTS}")
    message(FATAL_ERROR
            "Step 4 Ninja object inventory differs for ${STEP4_TARGET}: actual=[${ACTUAL_TARGET_OBJECTS}] expected=[${EXPECTED_TARGET_OBJECTS}]")
  endif()
endforeach()
list(APPEND EXPECTED_RAW_DEPENDENT_OBJECTS
     "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/real_differential/StaCsrRealReferenceProbe.cpp.o"
     "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/CsrReferencePoisonSupport.cpp.o"
     "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/CsrReferenceInputRawProjector.cpp.o"
     "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/CsrReferenceInputRawConstraintProjector.cpp.o"
     "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/CsrReferenceInputRawLibraryProjector.cpp.o"
     "src/operation/iSTA/test/CMakeFiles/ista_csr_real_reference_probe.dir/CsrReferenceInputRawParasiticProjector.cpp.o")
list(SORT ACTUAL_STEP4_TARGETS)
list(SORT EXPECTED_STEP4_TARGETS)
if(NOT "${ACTUAL_STEP4_TARGETS}" STREQUAL "${EXPECTED_STEP4_TARGETS}")
  message(FATAL_ERROR
          "Step 4 target inventory differs: actual=[${ACTUAL_STEP4_TARGETS}] expected=[${EXPECTED_STEP4_TARGETS}]")
endif()
list(LENGTH EXPECTED_STEP4_OBJECTS STEP4_OBJECT_COUNT)
if(NOT STEP4_OBJECT_COUNT EQUAL 71)
  message(FATAL_ERROR "Step 4 object inventory must contain exactly 71 objects: ${STEP4_OBJECT_COUNT}")
endif()

execute_process(
  COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps
  RESULT_VARIABLE ALL_DEPS_RESULT
  OUTPUT_VARIABLE ALL_DEPS_OUTPUT
  ERROR_VARIABLE ALL_DEPS_ERROR
)
if(NOT ALL_DEPS_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect global Ninja dependencies: ${ALL_DEPS_ERROR}")
endif()
set(RAW_HEADER "${CSR_TEST_ROOT}/CsrReferenceInputRawProjection.hpp")
string(REPLACE "\n" ";" ALL_DEPS_LINES "${ALL_DEPS_OUTPUT}")
set(ACTUAL_RAW_DEPENDENT_OBJECTS)
set(CURRENT_RAW_DEPS_OWNER "")
foreach(ALL_DEPS_LINE IN LISTS ALL_DEPS_LINES)
  if(ALL_DEPS_LINE MATCHES "^([^ \\t][^:]*\\.o): #deps .*\\(VALID\\)")
    set(CURRENT_RAW_DEPS_OWNER "${CMAKE_MATCH_1}")
  elseif(ALL_DEPS_LINE MATCHES "^[^ \\t][^:]*\\.o:")
    set(CURRENT_RAW_DEPS_OWNER "")
  else()
    string(STRIP "${ALL_DEPS_LINE}" STRIPPED_DEPS_LINE)
    if(STRIPPED_DEPS_LINE STREQUAL RAW_HEADER)
      if(CURRENT_RAW_DEPS_OWNER STREQUAL "")
        message(FATAL_ERROR "raw DTO dependency has no valid owning Ninja object")
      endif()
      list(APPEND ACTUAL_RAW_DEPENDENT_OBJECTS "${CURRENT_RAW_DEPS_OWNER}")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES ACTUAL_RAW_DEPENDENT_OBJECTS)
list(SORT ACTUAL_RAW_DEPENDENT_OBJECTS)
list(SORT EXPECTED_RAW_DEPENDENT_OBJECTS)
if(NOT "${ACTUAL_RAW_DEPENDENT_OBJECTS}" STREQUAL "${EXPECTED_RAW_DEPENDENT_OBJECTS}")
  message(FATAL_ERROR
          "raw DTO compiled-dependency inventory differs: actual=[${ACTUAL_RAW_DEPENDENT_OBJECTS}] expected=[${EXPECTED_RAW_DEPENDENT_OBJECTS}]")
endif()

file(GLOB_RECURSE REFERENCE_ARTIFACTS LIST_DIRECTORIES TRUE "${REFERENCE_DIR}/*")
foreach(REFERENCE_ARTIFACT IN LISTS REFERENCE_ARTIFACTS)
  if(REFERENCE_ARTIFACT MATCHES "/__pycache__$" OR REFERENCE_ARTIFACT MATCHES "\\.pyc$")
    message(FATAL_ERROR "Python bytecode artifact is prohibited: ${REFERENCE_ARTIFACT}")
  endif()
endforeach()

if(EXISTS "${PROJECTION_DIR}/StaCsrReferenceInputProbe.cpp")
  message(FATAL_ERROR "Step 4 probe must exist only in reference_input/")
endif()

message(STATUS "CSR Step 4 test-only, Python, and projection isolation passed")
