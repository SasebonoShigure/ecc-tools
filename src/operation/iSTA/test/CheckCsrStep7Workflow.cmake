# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

set(EXPECTED_PROBE_SHA256
    "ce5f3cdc6baf7bbd90ecbf62aab843191d4fde3d7e3b1f7037758a18e08a9bff")
set(EXPECTED_COMPAT_SHA256
    "0da3a1b22ffd0ece8198c97854a78e9db342af42c5daad45ebfafb1c9f8fb414")
set(EXPECTED_SESSION_CONTRACT_SHA256
    "0ff13a91b9bb741215f6186ae1c5ce935e920c529ab075c5771a42500862698a")
set(EXPECTED_SESSION_CONTROLLER_SHA256
    "8c095eab9159b372a48d4ed97f4a11ded9ac37122ed3c5ce4a923980512d5fab")
set(EXPECTED_SESSION_SELFTEST_SHA256
    "515dc87189027524447d79bc1d459b38c3b5bd64c5f7730578b811c8024916c4")

function(require_step7_workflow_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 7 workflow file: ${FILE_PATH}")
  endif()
endfunction()

function(read_step7_workflow_file OUTPUT FILE_PATH)
  require_step7_workflow_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(require_step7_workflow_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
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
            "Step 7 workflow detector rejected ${DESCRIPTION}: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

function(require_step7_workflow_order CONTENT_VARIABLE DESCRIPTION)
  set(CURSOR 0)
  foreach(TOKEN IN LISTS ARGN)
    string(SUBSTRING "${${CONTENT_VARIABLE}}" ${CURSOR} -1 TAIL)
    string(FIND "${TAIL}" "${TOKEN}" RELATIVE)
    if(RELATIVE EQUAL -1)
      message(FATAL_ERROR
              "Step 7 workflow detector rejected ${DESCRIPTION}: missing/out-of-order token ${TOKEN}")
    endif()
    string(LENGTH "${TOKEN}" TOKEN_LENGTH)
    math(EXPR CURSOR "${CURSOR} + ${RELATIVE} + ${TOKEN_LENGTH}")
  endforeach()
endfunction()

function(validate_step7_compat FILE_PATH)
  read_step7_workflow_file(COMPAT "${FILE_PATH}")
  string(JSON ROOT_LENGTH LENGTH "${COMPAT}")
  if(NOT ROOT_LENGTH EQUAL 4)
    message(FATAL_ERROR "Step 7 workflow detector rejected compat root key count")
  endif()
  set(ROOT_KEYS)
  foreach(INDEX RANGE 0 3)
    string(JSON KEY MEMBER "${COMPAT}" ${INDEX})
    list(APPEND ROOT_KEYS "${KEY}")
  endforeach()
  set(EXPECTED_ROOT_KEYS
      baseline_probe_source_sha256 canonicalization payload_sha256_by_case schema)
  list(SORT ROOT_KEYS)
  list(SORT EXPECTED_ROOT_KEYS)
  if(NOT "${ROOT_KEYS}" STREQUAL "${EXPECTED_ROOT_KEYS}")
    message(FATAL_ERROR "Step 7 workflow detector rejected compat root keys")
  endif()
  string(JSON SCHEMA GET "${COMPAT}" schema)
  string(JSON BASELINE_SHA GET "${COMPAT}" baseline_probe_source_sha256)
  string(JSON CHAIN_SHA GET "${COMPAT}" payload_sha256_by_case chain)
  string(JSON DIAMOND_SHA GET "${COMPAT}" payload_sha256_by_case diamond)
  string(JSON REMOVED_COUNT LENGTH "${COMPAT}" canonicalization removed_root_keys)
  string(JSON REMOVED_0 GET "${COMPAT}" canonicalization removed_root_keys 0)
  string(JSON REMOVED_1 GET "${COMPAT}" canonicalization removed_root_keys 1)
  string(JSON SEPARATOR_0 GET "${COMPAT}" canonicalization separators 0)
  string(JSON SEPARATOR_1 GET "${COMPAT}" canonicalization separators 1)
  string(JSON SORT_KEYS GET "${COMPAT}" canonicalization sort_keys)
  string(JSON ENSURE_ASCII GET "${COMPAT}" canonicalization ensure_ascii)
  string(JSON FINAL_NEWLINE GET "${COMPAT}" canonicalization final_newline)
  if(NOT SCHEMA STREQUAL "ista-csr-projection-default-compat/1.0.0"
     OR NOT BASELINE_SHA STREQUAL
        "a91baa76ed378ba600621445d8ebe94f0b1636c56650da3f2a7e52f4f2ece64d"
     OR NOT CHAIN_SHA STREQUAL
        "aa90a070164f8561cadd68852ff34a88e4045c6b9a37902bab6304952028d9dd"
     OR NOT DIAMOND_SHA STREQUAL
        "61a87d3fedefdcb12bc7c8868dbb361ad512f5f573413290d15fad08890bfa1f"
     OR NOT REMOVED_COUNT EQUAL 2
     OR NOT REMOVED_0 STREQUAL run_nonce
     OR NOT REMOVED_1 STREQUAL repeat_index
     OR NOT SEPARATOR_0 STREQUAL ","
     OR NOT SEPARATOR_1 STREQUAL ":"
     OR NOT SORT_KEYS
     OR ENSURE_ASCII
     OR FINAL_NEWLINE)
    message(FATAL_ERROR "Step 7 workflow detector rejected compat schema/value")
  endif()
endfunction()

if(DEFINED CSR_STEP7_WORKFLOW_DETECTOR_MODE)
  if(CSR_STEP7_WORKFLOW_DETECTOR_MODE STREQUAL compat)
    validate_step7_compat("${CSR_STEP7_WORKFLOW_DETECTOR_FILE}")
  elseif(CSR_STEP7_WORKFLOW_DETECTOR_MODE STREQUAL tokens)
    read_step7_workflow_file(DETECTOR "${CSR_STEP7_WORKFLOW_DETECTOR_FILE}")
    string(REPLACE "|" ";" TOKENS "${CSR_STEP7_WORKFLOW_DETECTOR_TOKENS}")
    foreach(TOKEN IN LISTS TOKENS)
      require_step7_workflow_count(DETECTOR "${TOKEN}" 1 "workflow fixture token ${TOKEN}")
    endforeach()
  else()
    message(FATAL_ERROR "unknown Step 7 workflow detector mode")
  endif()
  return()
endif()

foreach(REQUIRED_VARIABLE ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

function(run_step7_workflow_detector MODE FILE_PATH TOKENS EXPECT_SUCCESS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              "-DCSR_STEP7_WORKFLOW_DETECTOR_MODE=${MODE}"
              "-DCSR_STEP7_WORKFLOW_DETECTOR_FILE=${FILE_PATH}"
              "-DCSR_STEP7_WORKFLOW_DETECTOR_TOKENS=${TOKENS}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 7 workflow safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 7 workflow detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 7 workflow poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

set(PROJECTION_DIR "${CSR_TEST_ROOT}/projection")
set(SESSION_DIR "${CSR_TEST_ROOT}/csr_session")
set(ADAPTER_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr")
set(COMPAT_FILE "${PROJECTION_DIR}/projection_default_compat_v1.json")
set(WORKFLOW_DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step7-workflow-detector")
file(REMOVE_RECURSE "${WORKFLOW_DETECTOR_DIR}")
file(MAKE_DIRECTORY "${WORKFLOW_DETECTOR_DIR}")
set(WORKFLOW_TOKENS
    "projection_controller.run_controller(|--probe-mode|.csr-session-result-|csr-session-result.json")
file(WRITE "${WORKFLOW_DETECTOR_DIR}/safe.txt"
     "projection_controller.run_controller(\n--probe-mode\n.csr-session-result-\ncsr-session-result.json\n")
file(WRITE "${WORKFLOW_DETECTOR_DIR}/flag-poison.txt"
     "projection_controller.run_controller(\n--probe-mode\n--probe-mode\n.csr-session-result-\ncsr-session-result.json\n")
file(WRITE "${WORKFLOW_DETECTOR_DIR}/protocol-poison.txt"
     "projection_controller.run_controller(\n--probe-mode\n.csr-session-result-\n")
run_step7_workflow_detector(
    tokens "${WORKFLOW_DETECTOR_DIR}/safe.txt" "${WORKFLOW_TOKENS}" TRUE)
run_step7_workflow_detector(
    tokens "${WORKFLOW_DETECTOR_DIR}/flag-poison.txt" "${WORKFLOW_TOKENS}" FALSE)
run_step7_workflow_detector(
    tokens "${WORKFLOW_DETECTOR_DIR}/protocol-poison.txt" "${WORKFLOW_TOKENS}" FALSE)
run_step7_workflow_detector(compat "${COMPAT_FILE}" unused TRUE)
read_step7_workflow_file(COMPAT_CONTENT "${COMPAT_FILE}")
string(REPLACE
       "aa90a070164f8561cadd68852ff34a88e4045c6b9a37902bab6304952028d9dd"
       "0a90a070164f8561cadd68852ff34a88e4045c6b9a37902bab6304952028d9dd"
       COMPAT_POISON "${COMPAT_CONTENT}")
file(WRITE "${WORKFLOW_DETECTOR_DIR}/compat-poison.json" "${COMPAT_POISON}")
run_step7_workflow_detector(
    compat "${WORKFLOW_DETECTOR_DIR}/compat-poison.json" unused FALSE)
file(REMOVE_RECURSE "${WORKFLOW_DETECTOR_DIR}")
if(EXISTS "${WORKFLOW_DETECTOR_DIR}")
  message(FATAL_ERROR "Step 7 workflow detector cleanup failed")
endif()

function(require_step7_hash FILE_PATH EXPECTED DESCRIPTION)
  require_step7_workflow_file("${FILE_PATH}")
  file(SHA256 "${FILE_PATH}" ACTUAL)
  if(NOT ACTUAL STREQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 7 ${DESCRIPTION} hash differs: actual=${ACTUAL}, expected=${EXPECTED}")
  endif()
endfunction()

require_step7_hash(
    "${PROJECTION_DIR}/projection_controller.py"
    "5ec34f6920da457823382ad15d1eb7e728cc06b6db80e15d470977db7e07c011"
    "legacy projection controller")
require_step7_hash(
    "${PROJECTION_DIR}/projection_controller_selftest.py"
    "618252410083edb07f4f14f7307f6aeb85d52043280f19bd6cc5653d806e73ee"
    "legacy projection controller selftest")
require_step7_hash(
    "${PROJECTION_DIR}/projection_contract.py"
    "f91865827c4653134c2b73c557c5f1fef95252419558c28958954270a20b8acd"
    "legacy projection contract")
require_step7_hash(
    "${PROJECTION_DIR}/projection_canonical.py"
    "f9aef86fdf8903a52449508f0e88d648d15b7096e074cf4eabaa83cd38f64acb"
    "legacy projection canonicalizer")
require_step7_hash("${COMPAT_FILE}" "${EXPECTED_COMPAT_SHA256}" "default compat")
require_step7_hash(
    "${SESSION_DIR}/csr_session_contract.py"
    "${EXPECTED_SESSION_CONTRACT_SHA256}" "session contract")
require_step7_hash(
    "${SESSION_DIR}/csr_session_controller.py"
    "${EXPECTED_SESSION_CONTROLLER_SHA256}" "session controller")
require_step7_hash(
    "${SESSION_DIR}/csr_session_controller_selftest.py"
    "${EXPECTED_SESSION_SELFTEST_SHA256}" "session controller selftest")
if(EXPECTED_PROBE_SHA256 MATCHES "^__")
  message(FATAL_ERROR "Step 7 probe SHA placeholder was not replaced")
endif()
set(PROBE "${PROJECTION_DIR}/StaCsrProjectionProbe.cpp")
require_step7_hash("${PROBE}" "${EXPECTED_PROBE_SHA256}" "session projection probe")
validate_step7_compat("${COMPAT_FILE}")

file(GLOB SESSION_PYTHON LIST_DIRECTORIES FALSE "${SESSION_DIR}/*.py")
set(EXPECTED_SESSION_PYTHON
    "${SESSION_DIR}/csr_session_contract.py"
    "${SESSION_DIR}/csr_session_controller.py"
    "${SESSION_DIR}/csr_session_controller_selftest.py")
list(SORT SESSION_PYTHON)
list(SORT EXPECTED_SESSION_PYTHON)
if(NOT "${SESSION_PYTHON}" STREQUAL "${EXPECTED_SESSION_PYTHON}")
  message(FATAL_ERROR "Step 7 session Python inventory differs: [${SESSION_PYTHON}]")
endif()
if(EXISTS "${CSR_TEST_ROOT}/StaCsrSessionProbe.cpp")
  message(FATAL_ERROR "Step 7 must reuse the projection probe target/source")
endif()

read_step7_workflow_file(CONTROLLER "${SESSION_DIR}/csr_session_controller.py")
foreach(TOKEN
        "projection_controller.run_controller("
        "run_root=run_root.resolve().parent / \"projection-runs\""
        "repeats=3"
        "timeout_seconds=timeout_seconds"
        "\"--probe-mode\""
        "\"session\""
        ".csr-session-result-"
        "csr-session-result.json"
        ".csr-session-controller-"
        "csr-session-controller-result.json")
  require_step7_workflow_count(CONTROLLER "${TOKEN}" 1 "controller token ${TOKEN}")
endforeach()
require_step7_workflow_count(
    CONTROLLER "subprocess.run(" 1 "session-only subprocess implementation")
require_step7_workflow_order(
    CONTROLLER "legacy default phase before session phase"
    "projection_controller.run_controller("
    "validate_default_records("
    "_create_unique_run_directory(run_root)"
    "_prepare_slots("
    "run_session_slot("
    "_build_summary("
    "publish_atomic(temporary, final, \"session controller summary\")")
require_step7_workflow_order(
    CONTROLLER "session strict temp/final protocol"
    "_assert_absent(slot.temporary_result"
    "_assert_absent(slot.final_result"
    "subprocess.run("
    "_assert_absent(slot.final_result"
    "require_regular_owned_file(slot.temporary_result"
    "load_json(slot.temporary_result)"
    "validate_session_probe_record("
    "publish_atomic(slot.temporary_result, slot.final_result")

read_step7_workflow_file(PROBE_CONTENT "${PROBE}")
require_step7_workflow_count(
    PROBE_CONTENT "exportCsrReferenceInputCandidate(" 1
    "probe direct exporter oracle")
require_step7_workflow_count(
    PROBE_CONTENT "replayFifoLevelization(" 1
    "probe independent FIFO oracle")
require_step7_workflow_count(
    PROBE_CONTENT "STADM.getDatabase(" 0 "probe direct Database access")
require_step7_workflow_count(
    PROBE_CONTENT "inspectSessionDatabaseForTesting(token, callback)" 1
    "probe guarded Database friend callsite")
require_step7_workflow_count(
    PROBE_CONTENT "\"ista-csr-session-probe/1.0.0\"" 1
    "probe session schema")
require_step7_workflow_order(
    PROBE_CONTENT "guarded independent session oracle callback"
    "SessionOracle observeSessionDatabase("
    "exportCsrReferenceInputCandidate("
    "replayFifoLevelization("
    "return oracle;"
    "inspectDatabase(token, [&](const Database& database)"
    "oracle = observeSessionDatabase(database, token)")
require_step7_workflow_count(
    PROBE_CONTENT "\"--probe-mode\"" 1 "optional probe-mode parser")
require_step7_workflow_count(
    PROBE_CONTENT "probe_mode->second != \"session\"" 1 "exact session mode")

get_filename_component(CHILD_ROOT "${ISTA_SOURCE_ROOT}/../../.." ABSOLUTE)
get_filename_component(PARENT_ROOT "${CHILD_ROOT}/../../.." ABSOLUTE)
set(PARENT_STEP7_AVAILABLE FALSE)
if(EXISTS "${PARENT_ROOT}/chipcompiler/tools/ecc/sta_session.py")
  set(PARENT_STEP7_AVAILABLE TRUE)
endif()
set(ACTION "${CHILD_ROOT}/.github/actions/build-wheel/action.yml")
read_step7_workflow_file(ACTION_CONTENT "${ACTION}")
foreach(TARGET
        ista_csr_session_snapshot_adapter_contract_test
        ista_csr_session_context_test
        ista_csr_session_lifecycle_test
        ista_csr_snapshot_binding_contract_test
        ista_csr_projection_probe
        py_ista
        ecc_py)
  require_step7_workflow_count(
      ACTION_CONTENT "${TARGET}" 1 "wheel explicit target ${TARGET}")
endforeach()
require_step7_workflow_count(
    ACTION_CONTENT "--no-tests=error" 3 "wheel three label CTests")
foreach(LABEL ista_unit ista_property ista_smoke)
  require_step7_workflow_count(
      ACTION_CONTENT "-L '^${LABEL}$'" 1 "wheel exact ${LABEL} CTest")
endforeach()
require_step7_workflow_count(
    ACTION_CONTENT "test/csr_session/__pycache__" 1
    "wheel session bytecode rejection")
require_step7_workflow_count(
    ACTION_CONTENT "'src/**/*.cmake'" 1 "wheel CMake include cache key")

function(step7_line_count FILE_PATH OUTPUT)
  require_step7_workflow_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  string(REGEX REPLACE "[^\n]" "" LINE_BREAKS "${CONTENT}")
  string(LENGTH "${LINE_BREAKS}" LINE_COUNT)
  string(LENGTH "${CONTENT}" CONTENT_LENGTH)
  if(CONTENT_LENGTH GREATER 0)
    math(EXPR LAST_INDEX "${CONTENT_LENGTH} - 1")
    string(SUBSTRING "${CONTENT}" ${LAST_INDEX} 1 LAST_CHARACTER)
    if(NOT LAST_CHARACTER STREQUAL "\n")
      math(EXPR LINE_COUNT "${LINE_COUNT} + 1")
    endif()
  endif()
  set("${OUTPUT}" "${LINE_COUNT}" PARENT_SCOPE)
endfunction()

function(require_step7_under_700 FILE_PATH)
  step7_line_count("${FILE_PATH}" LINE_COUNT)
  if(LINE_COUNT GREATER_EQUAL 700)
    message(STATUS
            "Step 7 modified/new file advisory line target reached or exceeded: ${FILE_PATH}: ${LINE_COUNT}")
  endif()
endfunction()

set(STEP7_SMALL_FILES
    "${ISTA_SOURCE_ROOT}/benchmark/schema/validate_incremental_sta_samples.py"
    "${ISTA_SOURCE_ROOT}/source/CMakeLists.txt"
    "${ISTA_SOURCE_ROOT}/source/data_manager/CMakeLists.txt"
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
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingPathState.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingPoint.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingPortConstraint.hpp"
    "${ISTA_SOURCE_ROOT}/source/data_manager/advance/TimingTable.hpp"
    "${ISTA_SOURCE_ROOT}/source/module/CMakeLists.txt"
    "${ISTA_SOURCE_ROOT}/source/module/sdf_writer/SDFWriter.hpp"
    "${CSR_TEST_ROOT}/ArcTimingArcDelayTest.cpp"
    "${CSR_TEST_ROOT}/IncrementalStaSessionTest.cpp"
    "${CSR_TEST_ROOT}/PythonBindingContractTest.py"
    "${CSR_TEST_ROOT}/StaProductionLifecycleTest.cpp"
    "${CSR_TEST_ROOT}/baseline/StaFullBaselineProbe.cpp"
    "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CsrSnapshotIdentityValidator.cpp"
    "${ADAPTER_DIR}/CsrSessionSnapshotAdapter.hpp"
    "${ADAPTER_DIR}/CsrSessionSnapshotAdapterInternal.hpp"
    "${ADAPTER_DIR}/CsrSessionSnapshotAdapter.cpp"
    "${ADAPTER_DIR}/TimingResultScatter.cpp"
    "${ADAPTER_DIR}/TimingResultScatterInternal.hpp"
    "${ADAPTER_DIR}/TimingResultScatterPublish.cpp"
    "${ADAPTER_DIR}/CMakeLists.txt"
    "${ISTA_SOURCE_ROOT}/interface/StaCsrSessionContext.hpp"
    "${ISTA_SOURCE_ROOT}/interface/StaCsrSessionContext.cpp"
    "${ISTA_SOURCE_ROOT}/interface/IncrementalStaSession.hpp"
    "${ISTA_SOURCE_ROOT}/interface/IncrementalStaSession.cpp"
    "${ISTA_SOURCE_ROOT}/interface/STAInterface.hpp"
    "${ISTA_SOURCE_ROOT}/interface/STAInterfaceOperations.cpp"
    "${ISTA_SOURCE_ROOT}/interface/StaLifecycleGuard.hpp"
    "${ISTA_SOURCE_ROOT}/interface/StaLifecycleGuard.cpp"
    "${ISTA_SOURCE_ROOT}/interface/CMakeLists.txt"
    "${CHILD_ROOT}/src/interface/python/py_ista/py_ista.h"
    "${CHILD_ROOT}/src/interface/python/py_ista/py_ista.cpp"
    "${CHILD_ROOT}/src/interface/python/py_ista/py_ista_snapshot.h"
    "${CHILD_ROOT}/src/interface/python/py_ista/py_register_ista.h"
    "${CHILD_ROOT}/src/interface/python/py_ista/CMakeLists.txt"
    "${CSR_TEST_ROOT}/CsrSessionSnapshotAdapterContractTest.cpp"
    "${CSR_TEST_ROOT}/StaCsrSessionContextTest.cpp"
    "${CSR_TEST_ROOT}/StaCsrSessionLifecycleTest.cpp"
    "${CSR_TEST_ROOT}/StaCsrSnapshotBindingContractTest.cpp"
    "${CSR_TEST_ROOT}/CsrSessionTests.cmake"
    "${CSR_TEST_ROOT}/CMakeLists.txt"
    "${CSR_TEST_ROOT}/CheckCsrStep7SourceInventory.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep3SourceInventory.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep4SourceInventory.cmake"
    "${CSR_TEST_ROOT}/CheckCsrReferenceInputIsolation.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep7Targets.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep7Dependencies.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep7GuardPublish.cmake"
    "${CSR_TEST_ROOT}/CheckCsrStep7Workflow.cmake"
    "${PROBE}"
    "${COMPAT_FILE}"
    "${SESSION_DIR}/csr_session_contract.py"
    "${SESSION_DIR}/csr_session_controller.py"
    "${SESSION_DIR}/csr_session_controller_selftest.py"
    "${ACTION}")
if(PARENT_STEP7_AVAILABLE)
  list(APPEND STEP7_SMALL_FILES
      "${PARENT_ROOT}/chipcompiler/tools/ecc/sta_session.py"
      "${PARENT_ROOT}/chipcompiler/tools/ecc/_sta_snapshot_validation.py"
      "${PARENT_ROOT}/chipcompiler/tools/ecc/sta_signoff.py"
      "${PARENT_ROOT}/chipcompiler/tools/ecc/sta_snapshot.py"
      "${PARENT_ROOT}/test/tools/ecc/test_module_timing.py"
      "${PARENT_ROOT}/test/tools/ecc/test_runner.py"
      "${PARENT_ROOT}/test/tools/ecc/test_runner_sta_mode.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_session.py"
      "${PARENT_ROOT}/test/tools/ecc/test_module_timing_snapshot.py"
      "${PARENT_ROOT}/test/tools/ecc/test_runner_sta_snapshot.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_session_snapshot.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_signoff.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_snapshot.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_snapshot_context.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_snapshot_schema.py"
      "${PARENT_ROOT}/test/tools/ecc/test_sta_snapshot_transition.py")
endif()
foreach(FILE IN LISTS STEP7_SMALL_FILES)
  require_step7_under_700("${FILE}")
endforeach()
if(PARENT_STEP7_AVAILABLE)
  set(PARENT_RUNNER "${PARENT_ROOT}/test/tools/ecc/test_runner.py")
  require_step7_hash("${PARENT_RUNNER}" "6845d6ad7b73dce116e61602cc846bea186e39f85c46d21eda96dc4437035805" "mechanically split parent runner")
  step7_line_count("${PARENT_RUNNER}" PARENT_RUNNER_LINES)
  if(NOT PARENT_RUNNER_LINES EQUAL 413)
    message(FATAL_ERROR "Step 7 parent runner line count differs: ${PARENT_RUNNER_LINES}")
  endif()
endif()

read_step7_workflow_file(LARGE_INTERFACE "${ISTA_SOURCE_ROOT}/interface/STAInterface.cpp")
foreach(MOVED_DEFINITION
        "STAInterface::initializeOwned(" "STAInterface::runOwned("
        "STAInterface::closeOwned(" "STAInterface::acquireUse("
        "STAInterface::copySessionCsrSnapshotObservation(")
  require_step7_workflow_count(
      LARGE_INTERFACE "${MOVED_DEFINITION}" 0
      "large-file moved definition ${MOVED_DEFINITION}")
endforeach()

file(GLOB_RECURSE STEP7_ARTIFACTS LIST_DIRECTORIES TRUE
     "${SESSION_DIR}/*" "${PROJECTION_DIR}/*"
     "${ADAPTER_DIR}/CsrSessionSnapshotAdapter*"
     "${CSR_TEST_ROOT}/CheckCsrStep7*")
foreach(ARTIFACT IN LISTS STEP7_ARTIFACTS)
  if(ARTIFACT MATCHES
     "(__pycache__|\\.pyc$|\\.o$|\\.d$|\\.patch$|\\.rej$|\\.orig$|\\.bak$|~$)")
    message(FATAL_ERROR "Step 7 source-tree artifact leakage: ${ARTIFACT}")
  endif()
endforeach()

message(STATUS "CSR Step 7 workflow, compatibility, hashes, wheel, and line counts passed")
