# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

function(require_step7_dependency_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 7 dependency file: ${FILE_PATH}")
  endif()
endfunction()

function(read_step7_dependency_file OUTPUT FILE_PATH)
  require_step7_dependency_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(require_step7_dependency_absent CONTENT_VARIABLE TOKEN DESCRIPTION)
  string(FIND "${${CONTENT_VARIABLE}}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR
            "Step 7 dependency detector rejected ${DESCRIPTION}: prohibited token ${TOKEN}")
  endif()
endfunction()

function(require_step7_dependency_present CONTENT_VARIABLE TOKEN DESCRIPTION)
  string(FIND "${${CONTENT_VARIABLE}}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR
            "Step 7 dependency detector rejected ${DESCRIPTION}: missing token ${TOKEN}")
  endif()
endfunction()

function(check_step7_snapshot_allowlist CONTENT_VARIABLE ALLOWED OWNER)
  string(FIND "${${CONTENT_VARIABLE}}" "/source/module/timing_csr_snapshot/" SNAPSHOT_DIR)
  string(FIND "${${CONTENT_VARIABLE}}" "CsrSnapshotObservation.hpp" SNAPSHOT_HEADER)
  if(ALLOWED)
    if(SNAPSHOT_DIR EQUAL -1 AND SNAPSHOT_HEADER EQUAL -1)
      message(FATAL_ERROR
              "Step 7 dependency detector rejected allowed owner without snapshot dependency: ${OWNER}")
    endif()
  elseif(NOT SNAPSHOT_DIR EQUAL -1 OR NOT SNAPSHOT_HEADER EQUAL -1)
    message(FATAL_ERROR
            "Step 7 dependency detector rejected snapshot dependency outside allow-list: ${OWNER}")
  endif()
endfunction()

if(DEFINED CSR_STEP7_DEPENDENCY_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP7_DEPENDENCY_DETECTOR_FILE CSR_STEP7_DEPENDENCY_DETECTOR_ALLOWED)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  read_step7_dependency_file(DETECTOR_CONTENT "${CSR_STEP7_DEPENDENCY_DETECTOR_FILE}")
  check_step7_snapshot_allowlist(
      DETECTOR_CONTENT "${CSR_STEP7_DEPENDENCY_DETECTOR_ALLOWED}" detector-fixture)
  return()
endif()

foreach(REQUIRED_VARIABLE
        ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR NINJA_EXECUTABLE CSR_GENERATOR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

function(run_step7_dependency_detector FILE_PATH ALLOWED EXPECT_SUCCESS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              -DCSR_STEP7_DEPENDENCY_DETECTOR_MODE=TRUE
              "-DCSR_STEP7_DEPENDENCY_DETECTOR_FILE=${FILE_PATH}"
              "-DCSR_STEP7_DEPENDENCY_DETECTOR_ALLOWED=${ALLOWED}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 7 dependency safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 7 dependency detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 7 dependency poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

set(DEPENDENCY_DETECTOR_DIR "${CSR_BUILD_DIR}/csr-step7-dependency-detector")
set(DEPENDENCY_SAFE "${DEPENDENCY_DETECTOR_DIR}/safe.txt")
set(DEPENDENCY_SNAPSHOT "${DEPENDENCY_DETECTOR_DIR}/snapshot.txt")
file(REMOVE_RECURSE "${DEPENDENCY_DETECTOR_DIR}")
file(MAKE_DIRECTORY "${DEPENDENCY_DETECTOR_DIR}")
file(WRITE "${DEPENDENCY_SAFE}" "/project/source/data_manager/advance/Database.hpp\n")
file(WRITE "${DEPENDENCY_SNAPSHOT}"
     "/project/source/module/timing_csr_snapshot/CsrSnapshotObservation.hpp\n")
run_step7_dependency_detector("${DEPENDENCY_SAFE}" FALSE TRUE)
run_step7_dependency_detector("${DEPENDENCY_SNAPSHOT}" TRUE TRUE)
run_step7_dependency_detector("${DEPENDENCY_SNAPSHOT}" FALSE FALSE)
run_step7_dependency_detector("${DEPENDENCY_SAFE}" TRUE FALSE)
file(REMOVE_RECURSE "${DEPENDENCY_DETECTOR_DIR}")
if(EXISTS "${DEPENDENCY_DETECTOR_DIR}")
  message(FATAL_ERROR "Step 7 dependency detector cleanup failed")
endif()

set(ADAPTER_DIR "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr")
set(SNAPSHOT_DIR "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot")
set(INTERFACE_DIR "${ISTA_SOURCE_ROOT}/interface")
get_filename_component(CHILD_SRC_ROOT "${ISTA_SOURCE_ROOT}/../.." ABSOLUTE)
set(PY_DIR "${CHILD_SRC_ROOT}/interface/python")
set(PY_ISTA_DIR "${PY_DIR}/py_ista")

set(ADAPTER_SOURCES
    CsrGraphEncoding.cpp CsrGraphValidator.cpp TimingGraphCsrAdapter.cpp
    TimingGraphCsrArcExport.cpp TimingGraphCsrAdjacency.cpp NativeGraphProjection.cpp
    CsrReferenceInputAdapter.cpp TimingResultScatter.cpp
    TimingResultScatterValidation.cpp TimingResultScatterPreparation.cpp
    TimingResultScatterPublish.cpp CsrConstraintExport.cpp CsrClockSeedExport.cpp
    CsrTimingCheckExport.cpp CsrLibertyExport.cpp CsrLoadExport.cpp
    CsrParasiticExport.cpp CsrSessionSnapshotAdapter.cpp)
set(SNAPSHOT_ALLOWED_ADAPTER_SOURCES
    CsrSessionSnapshotAdapter.cpp TimingResultScatterPublish.cpp)
foreach(SOURCE IN LISTS ADAPTER_SOURCES)
  read_step7_dependency_file(SOURCE_CONTENT "${ADAPTER_DIR}/${SOURCE}")
  list(FIND SNAPSHOT_ALLOWED_ADAPTER_SOURCES "${SOURCE}" ALLOWED_INDEX)
  if(ALLOWED_INDEX EQUAL -1)
    foreach(TOKEN CsrSnapshotObservation.hpp CsrSnapshotObservationTransition.hpp
                  CsrSnapshotStorage.hpp CsrSessionSnapshotAdapterInternal.hpp)
      require_step7_dependency_absent(
          SOURCE_CONTENT "${TOKEN}" "old adapter source ${SOURCE}")
    endforeach()
  endif()
  foreach(INTERFACE_TOKEN STAInterface IncrementalStaSession StaCsrSessionContext)
    require_step7_dependency_absent(
        SOURCE_CONTENT "${INTERFACE_TOKEN}" "adapter-to-interface edge ${SOURCE}")
  endforeach()
endforeach()

file(GLOB ADAPTER_HEADERS LIST_DIRECTORIES FALSE "${ADAPTER_DIR}/*.hpp")
foreach(HEADER IN LISTS ADAPTER_HEADERS)
  read_step7_dependency_file(HEADER_CONTENT "${HEADER}")
  foreach(INTERFACE_TOKEN STAInterface IncrementalStaSession StaCsrSessionContext)
    require_step7_dependency_absent(
        HEADER_CONTENT "${INTERFACE_TOKEN}" "adapter-header-to-interface edge ${HEADER}")
  endforeach()
endforeach()

set(INTERFACE_IMPLEMENTATIONS
    IncrementalStaSession.cpp STAInterface.cpp STAInterfaceOperations.cpp
    StaCsrSessionContext.cpp StaLifecycleGuard.cpp StaProductionLifecycle.cpp
    StaExecutionContract.cpp)
foreach(SOURCE IN LISTS INTERFACE_IMPLEMENTATIONS)
  read_step7_dependency_file(SOURCE_CONTENT "${INTERFACE_DIR}/${SOURCE}")
  foreach(FORBIDDEN_HEADER
          CsrSessionSnapshotAdapterInternal.hpp CsrSnapshotObservation.hpp
          CsrSnapshotObservationTransition.hpp CsrSnapshotStorage.hpp
          CsrFifoLevelization.hpp CsrCpuReferenceBackend.hpp
          TimingResultScatterInternal.hpp)
    require_step7_dependency_absent(
        SOURCE_CONTENT "${FORBIDDEN_HEADER}" "interface naked lower-layer include ${SOURCE}")
  endforeach()
endforeach()
foreach(PUBLIC_HEADER IncrementalStaSession.hpp STAInterface.hpp)
  read_step7_dependency_file(PUBLIC_CONTENT "${INTERFACE_DIR}/${PUBLIC_HEADER}")
  foreach(FORBIDDEN_TOKEN
          CsrSessionSnapshotAdapter CsrSnapshotObservation.hpp
          CsrSnapshotObservationTransition TimingResultScatter.hpp
          CsrReferenceTypes.hpp)
    require_step7_dependency_absent(
        PUBLIC_CONTENT "${FORBIDDEN_TOKEN}" "public-header transitive edge ${PUBLIC_HEADER}")
  endforeach()
endforeach()

read_step7_dependency_file(INTERFACE_CMAKE "${INTERFACE_DIR}/CMakeLists.txt")
require_step7_dependency_present(
    INTERFACE_CMAKE ista_timing_graph_csr_adapter "interface direct adapter edge")
foreach(FORBIDDEN_LINK ista_timing_csr_core ista_timing_csr_snapshot_contract)
  require_step7_dependency_absent(
      INTERFACE_CMAKE "${FORBIDDEN_LINK}" "interface forbidden direct link")
endforeach()
read_step7_dependency_file(SOURCE_CMAKE "${ISTA_SOURCE_ROOT}/source/CMakeLists.txt")
read_step7_dependency_file(MODULE_CMAKE "${ISTA_SOURCE_ROOT}/source/module/CMakeLists.txt")
foreach(CONTENT_VARIABLE SOURCE_CMAKE MODULE_CMAKE)
  foreach(FORBIDDEN_AGGREGATE
          ista_timing_graph_csr_adapter ista_timing_csr_snapshot_contract)
    require_step7_dependency_absent(
        ${CONTENT_VARIABLE} "${FORBIDDEN_AGGREGATE}" "source/module aggregation")
  endforeach()
endforeach()

read_step7_dependency_file(PY_CMAKE "${PY_ISTA_DIR}/CMakeLists.txt")
require_step7_dependency_absent(PY_CMAKE aux_source_directory "py_ista implicit sources")
require_step7_dependency_present(
    PY_CMAKE "set(HOME_ISTA \${HOME_OPERATION}/iSTA)" "py_ista local iSTA root")

foreach(PRIVATE_INCLUDE
        "\${HOME_ISTA}/source/data_manager/adapter/csr"
        "\${HOME_ISTA}/source/module/timing_csr_snapshot")
  require_step7_dependency_present(
      PY_CMAKE "${PRIVATE_INCLUDE}" "py_ista PRIVATE include-only edge")
endforeach()
string(REGEX MATCH "target_include_directories\\(py_ista[^)]*\\)"
       PY_INCLUDE_BLOCK "${PY_CMAKE}")
if("${PY_INCLUDE_BLOCK}" STREQUAL "")
  message(FATAL_ERROR "missing py_ista include-directory block")
endif()
string(REPLACE "\"" "" PY_INCLUDE_BLOCK "${PY_INCLUDE_BLOCK}")
string(REGEX REPLACE "[ \t\r\n]+" ";" PY_INCLUDE_NORMALIZED "${PY_INCLUDE_BLOCK}")
set(EXPECTED_PY_INCLUDE_NORMALIZED
    [=[target_include_directories(py_ista;PUBLIC;${CMAKE_CURRENT_SOURCE_DIR};${HOME_UTILITY}/json;PRIVATE;${HOME_ISTA}/source/data_manager/adapter/csr;${HOME_ISTA}/source/module/timing_csr_snapshot;)]=])
if(NOT "${PY_INCLUDE_NORMALIZED}" STREQUAL "${EXPECTED_PY_INCLUDE_NORMALIZED}")
  message(FATAL_ERROR
          "py_ista exact PUBLIC/PRIVATE include inventory differs: [${PY_INCLUDE_NORMALIZED}]")
endif()
foreach(FORBIDDEN_LINK ista_timing_graph_csr_adapter ista_timing_csr_snapshot_contract)
  require_step7_dependency_absent(PY_CMAKE "${FORBIDDEN_LINK}" "py_ista forbidden direct link")
endforeach()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 7 dependency gate requires Ninja")
endif()
require_step7_dependency_file("${NINJA_EXECUTABLE}")
require_step7_dependency_file("${CSR_BUILD_DIR}/build.ninja")

function(read_step7_ninja_deps OBJECT SOURCE OUTPUT)
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${OBJECT}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE DEPS ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0 OR NOT DEPS MATCHES "\\(VALID\\)")
    message(FATAL_ERROR "missing valid Step 7 depfile: ${OBJECT}: ${DEPS}${ERROR}")
  endif()
  string(FIND "${DEPS}" "${SOURCE}" OWNER)
  if(OWNER EQUAL -1)
    message(FATAL_ERROR "Step 7 depfile owner mismatch: ${OBJECT} -> ${SOURCE}")
  endif()
  set("${OUTPUT}" "${DEPS}" PARENT_SCOPE)
endfunction()

set(ADAPTER_OBJECT_PREFIX
    "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/")
set(ACTUAL_SNAPSHOT_ADAPTER_OBJECTS)
foreach(SOURCE IN LISTS ADAPTER_SOURCES)
  set(OBJECT "${ADAPTER_OBJECT_PREFIX}${SOURCE}.o")
  read_step7_ninja_deps("${OBJECT}" "${ADAPTER_DIR}/${SOURCE}" DEPS)
  string(FIND "${DEPS}" "/source/module/timing_csr_snapshot/" SNAPSHOT_DEP)
  if(NOT SNAPSHOT_DEP EQUAL -1)
    list(APPEND ACTUAL_SNAPSHOT_ADAPTER_OBJECTS "${OBJECT}")
  endif()
endforeach()
set(EXPECTED_SNAPSHOT_ADAPTER_OBJECTS
    "${ADAPTER_OBJECT_PREFIX}CsrSessionSnapshotAdapter.cpp.o"
    "${ADAPTER_OBJECT_PREFIX}TimingResultScatterPublish.cpp.o")
list(SORT ACTUAL_SNAPSHOT_ADAPTER_OBJECTS)
list(SORT EXPECTED_SNAPSHOT_ADAPTER_OBJECTS)
if(NOT "${ACTUAL_SNAPSHOT_ADAPTER_OBJECTS}" STREQUAL
       "${EXPECTED_SNAPSHOT_ADAPTER_OBJECTS}")
  message(FATAL_ERROR
          "Step 7 adapter snapshot depfile allow-list differs: [${ACTUAL_SNAPSHOT_ADAPTER_OBJECTS}]")
endif()

set(PY_ISTA_OBJECT_PREFIX
    "src/interface/python/py_ista/CMakeFiles/py_ista.dir/")
set(BINDING_OBJECT
    "src/operation/iSTA/test/CMakeFiles/ista_csr_snapshot_binding_contract_test.dir/StaCsrSnapshotBindingContractTest.cpp.o")
set(BINDING_DEPENDENCY_ROWS
    "${PY_ISTA_OBJECT_PREFIX}py_ista.cpp.o|${PY_ISTA_DIR}/py_ista.cpp|TRUE"
    "${BINDING_OBJECT}|${CSR_TEST_ROOT}/StaCsrSnapshotBindingContractTest.cpp|TRUE"
    "${PY_ISTA_OBJECT_PREFIX}py_ista_utils.cpp.o|${PY_ISTA_DIR}/py_ista_utils.cpp|FALSE"
    "src/interface/python/CMakeFiles/ecc_py.dir/python_moodule.cc.o|${PY_DIR}/python_moodule.cc|FALSE")
foreach(ROW IN LISTS BINDING_DEPENDENCY_ROWS)
  string(REPLACE "|" ";" FIELDS "${ROW}")
  list(GET FIELDS 0 OBJECT)
  list(GET FIELDS 1 SOURCE)
  list(GET FIELDS 2 ALLOWED)
  read_step7_ninja_deps("${OBJECT}" "${SOURCE}" DEPS)
  string(FIND "${DEPS}" "/source/module/timing_csr_snapshot/" SNAPSHOT_DEP)
  string(FIND "${DEPS}" "/source/data_manager/adapter/csr/" ADAPTER_DEP)
  if(ALLOWED)
    if(SNAPSHOT_DEP EQUAL -1 OR ADAPTER_DEP EQUAL -1)
      message(FATAL_ERROR "binding allow-list object lacks adapter/snapshot deps: ${OBJECT}")
    endif()
  elseif(NOT SNAPSHOT_DEP EQUAL -1 OR NOT ADAPTER_DEP EQUAL -1)
    message(FATAL_ERROR "binding dependency escapes exact object allow-list: ${OBJECT}")
  endif()
endforeach()

message(STATUS "CSR Step 7 source, include, and compiled dependency isolation passed")
