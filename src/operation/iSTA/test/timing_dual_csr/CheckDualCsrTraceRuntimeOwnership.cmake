if(NOT DEFINED RUNTIME_DIR)
  message(FATAL_ERROR "RUNTIME_DIR is required")
endif()

set(RUNTIME_FILES
    "${RUNTIME_DIR}/DualCsrTraceBaseline.cpp"
    "${RUNTIME_DIR}/DualCsrNativeCandidateCarrier.cpp"
    "${RUNTIME_DIR}/DualCsrTraceReplayer.cpp"
)
set(CARRIER "${RUNTIME_DIR}/DualCsrNativeCandidateCarrier.cpp")
set(REPLAYER "${RUNTIME_DIR}/DualCsrTraceReplayer.cpp")
set(CANONICAL_PATTERNS
    "NativeDatabaseCandidate::create\\("
    "NativeDesignEditTranslator::applyNormalizedBatch\\("
    "NativeCandidateExecutor::installAndExecute\\("
    "installed\\.rollback\\(\\)"
)

foreach(SOURCE IN LISTS RUNTIME_FILES)
  if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "missing Stage 5 runtime source: ${SOURCE}")
  endif()
  file(READ "${SOURCE}" CONTENT)
  string(FIND "${CONTENT}" "captureNativeDualCsrPayload" RAW_CAPTURE)
  if(NOT RAW_CAPTURE EQUAL -1)
    message(FATAL_ERROR "runtime bypasses candidate-bound narrow reader: ${SOURCE}")
  endif()
  if(NOT SOURCE STREQUAL CARRIER)
    foreach(PATTERN IN LISTS CANONICAL_PATTERNS)
      string(REGEX MATCH "${PATTERN}" BYPASS "${CONTENT}")
      if(BYPASS)
        message(FATAL_ERROR
                "canonical native candidate call escaped the carrier: ${SOURCE}: ${PATTERN}")
      endif()
    endforeach()
  endif()
endforeach()

file(READ "${CARRIER}" CARRIER_CONTENT)
string(REGEX MATCHALL "NativeDatabaseCandidate::create\\(" CREATES
       "${CARRIER_CONTENT}")
list(LENGTH CREATES CREATE_COUNT)
if(NOT CREATE_COUNT EQUAL 2)
  message(FATAL_ERROR "carrier must own exactly create+clone candidate entries")
endif()
foreach(PATTERN IN ITEMS
    "NativeDesignEditTranslator::applyNormalizedBatch\\("
    "NativeCandidateExecutor::installAndExecute\\("
    "installed\\.rollback\\(\\)")
  string(REGEX MATCHALL "${PATTERN}" MATCHES "${CARRIER_CONTENT}")
  list(LENGTH MATCHES MATCH_COUNT)
  if(NOT MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "carrier canonical entry count differs: ${PATTERN}")
  endif()
endforeach()

file(READ "${REPLAYER}" REPLAYER_CONTENT)
string(REGEX MATCHALL "DualCsrNativeCandidateCarrier::(create|clone|apply|installAndExecute|rollback)\\("
       CARRIER_CALLS "${REPLAYER_CONTENT}")
list(LENGTH CARRIER_CALLS CARRIER_CALL_COUNT)
if(NOT CARRIER_CALL_COUNT EQUAL 6)
  message(FATAL_ERROR "replayer carrier lifecycle call inventory differs")
endif()
string(REGEX MATCHALL "NarrowTimingRecordReader\\{\\}\\.read\\("
       READER_CALLS "${REPLAYER_CONTENT}")
list(LENGTH READER_CALLS READER_CALL_COUNT)
if(NOT READER_CALL_COUNT EQUAL 1)
  message(FATAL_ERROR "replayer must use exactly one candidate-bound reader entry")
endif()
