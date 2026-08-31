# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

foreach(REQUIRED IN ITEMS ISTA_SOURCE_ROOT TRANSACTION_CLOSURE_PACKED)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "missing ${REQUIRED}")
  endif()
endforeach()

set(CONTRACT_FILES
    module/timing_edit_contract/TimingEditByteOrder.hpp
    module/timing_edit_contract/TimingEditCanonicalEncoding.cpp
    module/timing_edit_contract/TimingEditCanonicalEncoding.hpp
    module/timing_edit_contract/TimingEditPreparedBatchValidation.cpp
    module/timing_edit_contract/TimingEditTrace.cpp
    module/timing_edit_contract/TimingEditTrace.hpp
    module/timing_edit_contract/TimingEditTraceJson.cpp
    module/timing_edit_contract/TimingEditTypes.hpp
    module/timing_edit_contract/TimingEditValidation.cpp
    module/timing_edit_contract/TimingEditValidation.hpp
    module/timing_edit_contract/TimingEditValidationInternal.hpp
)
set(TRANSACTION_FILES
    module/timing_edit_transaction/CsrFullExactReceipt.cpp
    module/timing_edit_transaction/CsrFullExactReceipt.hpp
    module/timing_edit_transaction/TimingEditCoordinator.cpp
    module/timing_edit_transaction/TimingEditCoordinator.hpp
    module/timing_edit_transaction/TimingEditTransactionTypes.hpp
)

foreach(SOURCE IN LISTS CONTRACT_FILES)
  set(PATH "${ISTA_SOURCE_ROOT}/${SOURCE}")
  if(NOT EXISTS "${PATH}")
    message(FATAL_ERROR "timing-edit contract inventory is missing ${SOURCE}")
  endif()
  file(READ "${PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
          Database.hpp
          STAInterface.hpp
          IncrementalStaSession.hpp
          idm.h
          iZH
          CUDA
          cuda)
    string(FIND "${CONTENT}" "${FORBIDDEN}" OFFSET)
    if(NOT OFFSET EQUAL -1)
      message(FATAL_ERROR "timing-edit contract ${SOURCE} references forbidden dependency ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

foreach(SOURCE IN LISTS TRANSACTION_FILES)
  set(PATH "${ISTA_SOURCE_ROOT}/${SOURCE}")
  if(NOT EXISTS "${PATH}")
    message(FATAL_ERROR "timing-edit transaction inventory is missing ${SOURCE}")
  endif()
  file(READ "${PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
          STAInterface.hpp
          IncrementalStaSession.hpp
          idm.h
          iZH
          CUDA
          cuda)
    string(FIND "${CONTENT}" "${FORBIDDEN}" OFFSET)
    if(NOT OFFSET EQUAL -1)
      message(FATAL_ERROR "timing-edit transaction ${SOURCE} references forbidden dependency ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

string(REPLACE "|" ";" TRANSACTION_CLOSURE "${TRANSACTION_CLOSURE_PACKED}")
set(FORBIDDEN_TRANSITIVE_TARGETS py_ista idm)
foreach(DEPENDENCY IN LISTS TRANSACTION_CLOSURE)
  string(TOLOWER "${DEPENDENCY}" LOWER_DEPENDENCY)
  if(LOWER_DEPENDENCY MATCHES "izh|cuda")
    message(FATAL_ERROR "timing-edit transaction closure contains forbidden target ${DEPENDENCY}")
  endif()
  list(FIND FORBIDDEN_TRANSITIVE_TARGETS "${DEPENDENCY}" FORBIDDEN_INDEX)
  if(NOT FORBIDDEN_INDEX EQUAL -1)
    message(FATAL_ERROR
        "timing-edit transaction closure contains forbidden target ${DEPENDENCY}")
  endif()
endforeach()

# ista_timing_edit_native uses legacy full-timing stages. Their existing
# DataManager/toolkit closure currently carries ista_interface and ista_source.
# The exact direct-link inventory and source scans above enforce the Stage 3
# boundary without pretending that this pre-existing transitive closure is gone.
message(STATUS
    "timing-edit contract/transaction sources and direct links are isolated; legacy native timing closure is explicit")
