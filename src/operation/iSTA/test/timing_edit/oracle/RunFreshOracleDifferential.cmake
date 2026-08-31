# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

foreach(REQUIRED IN ITEMS
        ORACLE_EXECUTABLE PRODUCTION_EXECUTABLE PYTHON_EXECUTABLE
        TRACE_VALIDATOR TRACE_SCHEMA TECH_LEF CELL_LEF LIBERTY VERILOG SDC RUN_ROOT)
  if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
    message(FATAL_ERROR "missing ${REQUIRED}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${RUN_ROOT}")
set(TRACE_FILE "${RUN_ROOT}/timing-edit-trace.json")
set(COMMON_ARTIFACT_ARGS
    --tech-lef "${TECH_LEF}"
    --cell-lef "${CELL_LEF}"
    --liberty "${LIBERTY}"
    --verilog "${VERILOG}"
    --sdc "${SDC}"
    --top stage0_chain)

execute_process(
  COMMAND
    "${ORACLE_EXECUTABLE}"
    ${COMMON_ARTIFACT_ARGS}
    --run-dir "${RUN_ROOT}/trace-builder-run"
    --create-trace "${TRACE_FILE}"
  RESULT_VARIABLE TRACE_RESULT
  OUTPUT_VARIABLE TRACE_STDOUT
  ERROR_VARIABLE TRACE_STDERR
  TIMEOUT 180
)
if(NOT TRACE_RESULT EQUAL 0 OR NOT EXISTS "${TRACE_FILE}")
  message(FATAL_ERROR
    "durable trace generation failed (${TRACE_RESULT})\nstdout:\n${TRACE_STDOUT}\nstderr:\n${TRACE_STDERR}")
endif()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${PYTHON_EXECUTABLE}" "${TRACE_VALIDATOR}"
    --schema "${TRACE_SCHEMA}"
    --trace "${TRACE_FILE}"
  RESULT_VARIABLE SCHEMA_RESULT
  OUTPUT_VARIABLE SCHEMA_STDOUT
  ERROR_VARIABLE SCHEMA_STDERR
  TIMEOUT 60
)
if(NOT SCHEMA_RESULT EQUAL 0)
  message(FATAL_ERROR
    "generated trace failed the JSON schema/semantic validator (${SCHEMA_RESULT})\nstdout:\n${SCHEMA_STDOUT}\nstderr:\n${SCHEMA_STDERR}")
endif()

file(READ "${TRACE_FILE}" TRACE_TEXT)
foreach(REPLAY_EVIDENCE IN ITEMS
        "${ORACLE_EXECUTABLE}"
        "${TRACE_FILE}"
        "${RUN_ROOT}/recorded-replay-run"
        "${RUN_ROOT}/recorded-replay-observation.txt")
  string(FIND "${TRACE_TEXT}" "${REPLAY_EVIDENCE}" REPLAY_OFFSET)
  if(REPLAY_OFFSET EQUAL -1)
    message(FATAL_ERROR "durable replay_command omits ${REPLAY_EVIDENCE}")
  endif()
endforeach()

foreach(PREFIX RANGE 1 2)
  foreach(REPEAT RANGE 1 3)
    set(REPEAT_ROOT "${RUN_ROOT}/prefix-${PREFIX}/repeat-${REPEAT}")
    set(ORACLE_OUTPUT "${REPEAT_ROOT}/independent-observation.txt")
    set(PRODUCTION_OUTPUT "${REPEAT_ROOT}/production-observation.txt")
    file(MAKE_DIRECTORY "${REPEAT_ROOT}")

    execute_process(
      COMMAND
        "${ORACLE_EXECUTABLE}"
        ${COMMON_ARTIFACT_ARGS}
        --run-dir "${REPEAT_ROOT}/independent-run"
        --trace "${TRACE_FILE}"
        --prefix "${PREFIX}"
        --output "${ORACLE_OUTPUT}"
        --mode cpu/full
        --subject correct
      RESULT_VARIABLE ORACLE_RESULT
      OUTPUT_VARIABLE ORACLE_STDOUT
      ERROR_VARIABLE ORACLE_STDERR
      TIMEOUT 180
    )
    if(NOT ORACLE_RESULT EQUAL 0 OR NOT EXISTS "${ORACLE_OUTPUT}")
      message(FATAL_ERROR
        "independent prefix ${PREFIX} repeat ${REPEAT} failed (${ORACLE_RESULT})\nstdout:\n${ORACLE_STDOUT}\nstderr:\n${ORACLE_STDERR}")
    endif()

    execute_process(
      COMMAND
        "${PRODUCTION_EXECUTABLE}"
        ${COMMON_ARTIFACT_ARGS}
        --run-dir "${REPEAT_ROOT}/production-run"
        --trace "${TRACE_FILE}"
        --prefix "${PREFIX}"
        --output "${PRODUCTION_OUTPUT}"
        --mode cpu/full
        --subject correct
      RESULT_VARIABLE PRODUCTION_RESULT
      OUTPUT_VARIABLE PRODUCTION_STDOUT
      ERROR_VARIABLE PRODUCTION_STDERR
      TIMEOUT 180
    )
    set(CSR_RECEIPT "${PRODUCTION_OUTPUT}.csr-full.txt")
    if(NOT PRODUCTION_RESULT EQUAL 0 OR NOT EXISTS "${PRODUCTION_OUTPUT}" OR NOT EXISTS "${CSR_RECEIPT}")
      message(FATAL_ERROR
        "production prefix ${PREFIX} repeat ${REPEAT} failed (${PRODUCTION_RESULT})\nstdout:\n${PRODUCTION_STDOUT}\nstderr:\n${PRODUCTION_STDERR}")
    endif()

    file(READ "${ORACLE_OUTPUT}" ORACLE_TEXT)
    file(READ "${PRODUCTION_OUTPUT}" PRODUCTION_TEXT)
    file(READ "${CSR_RECEIPT}" CSR_RECEIPT_TEXT)
    foreach(CSR_ORDINAL RANGE 1 ${PREFIX})
      string(FIND "${CSR_RECEIPT_TEXT}"
        "csr_full_exact|${CSR_ORDINAL}|" CSR_RECEIPT_OFFSET)
      if(CSR_RECEIPT_OFFSET EQUAL -1)
        message(FATAL_ERROR
          "production prefix ${PREFIX} omitted CSR full exact receipt ${CSR_ORDINAL}")
      endif()
    endforeach()
    foreach(REQUIRED_RECORD IN ITEMS
            "instance|" "pin|" "net|" "point|" "arc|" "outgoing|" "incoming|"
            "order|" "electrical_load|" "path|")
      string(FIND "${ORACLE_TEXT}" "${REQUIRED_RECORD}" ORACLE_RECORD_OFFSET)
      string(FIND "${PRODUCTION_TEXT}" "${REQUIRED_RECORD}" PRODUCTION_RECORD_OFFSET)
      if(ORACLE_RECORD_OFFSET EQUAL -1 OR PRODUCTION_RECORD_OFFSET EQUAL -1)
        message(FATAL_ERROR
          "prefix ${PREFIX} repeat ${REPEAT} omits functional record ${REQUIRED_RECORD}")
      endif()
    endforeach()
    if(NOT "${PRODUCTION_TEXT}" STREQUAL "${ORACLE_TEXT}")
      message(FATAL_ERROR
        "production and independent prefix ${PREFIX} repeat ${REPEAT} differ field-by-field")
    endif()
    set(REFERENCE_VARIABLE "PREFIX_${PREFIX}_REFERENCE")
    if(REPEAT EQUAL 1)
      set("${REFERENCE_VARIABLE}" "${ORACLE_TEXT}")
    elseif(NOT "${ORACLE_TEXT}" STREQUAL
               "${${REFERENCE_VARIABLE}}")
      message(FATAL_ERROR
        "independent prefix ${PREFIX} repeat ${REPEAT} differs from repeat 1")
    endif()
  endforeach()
endforeach()

set(WRONG_ROOT "${RUN_ROOT}/wrong-sink-production")
set(WRONG_OUTPUT "${WRONG_ROOT}/observation.txt")
file(MAKE_DIRECTORY "${WRONG_ROOT}")
execute_process(
  COMMAND
    "${PRODUCTION_EXECUTABLE}"
    ${COMMON_ARTIFACT_ARGS}
    --run-dir "${WRONG_ROOT}/run"
    --trace "${TRACE_FILE}"
    --prefix 2
    --output "${WRONG_OUTPUT}"
    --mode cpu/full
    --subject wrong-sink
  RESULT_VARIABLE WRONG_RESULT
  OUTPUT_VARIABLE WRONG_STDOUT
  ERROR_VARIABLE WRONG_STDERR
  TIMEOUT 180
)
if(NOT WRONG_RESULT EQUAL 0 OR NOT EXISTS "${WRONG_OUTPUT}"
   OR NOT EXISTS "${WRONG_OUTPUT}.csr-full.txt")
  message(FATAL_ERROR
    "production wrong-sink subject did not execute (${WRONG_RESULT})\nstdout:\n${WRONG_STDOUT}\nstderr:\n${WRONG_STDERR}")
endif()
file(READ "${WRONG_OUTPUT}" WRONG_TEXT)
if("${WRONG_TEXT}" STREQUAL "${PREFIX_2_REFERENCE}")
  message(FATAL_ERROR "independent oracle did not detect the production wrong-sink subject")
endif()

foreach(UNSUPPORTED_MODE IN ITEMS cpu/incremental gpu/full gpu/incremental)
  string(REPLACE "/" "-" MODE_STEM "${UNSUPPORTED_MODE}")
  set(MODE_ROOT "${RUN_ROOT}/unsupported-${MODE_STEM}")
  set(MODE_OUTPUT "${MODE_ROOT}/status.txt")
  file(MAKE_DIRECTORY "${MODE_ROOT}")
  execute_process(
    COMMAND
      "${PRODUCTION_EXECUTABLE}"
      ${COMMON_ARTIFACT_ARGS}
      --run-dir "${MODE_ROOT}/run"
      --trace "${TRACE_FILE}"
      --prefix 2
      --output "${MODE_OUTPUT}"
      --mode "${UNSUPPORTED_MODE}"
      --subject correct
    RESULT_VARIABLE MODE_RESULT
    OUTPUT_VARIABLE MODE_STDOUT
    ERROR_VARIABLE MODE_STDERR
    TIMEOUT 60
  )
  if(NOT MODE_RESULT EQUAL 0 OR NOT EXISTS "${MODE_OUTPUT}")
    message(FATAL_ERROR
      "unsupported mode ${UNSUPPORTED_MODE} did not return a typed status (${MODE_RESULT})\nstdout:\n${MODE_STDOUT}\nstderr:\n${MODE_STDERR}")
  endif()
  file(READ "${MODE_OUTPUT}" MODE_TEXT)
  set(EXPECTED_MODE_TEXT
      "mode_status|${UNSUPPORTED_MODE}|unsupported|stage_not_implemented\n")
  if(NOT "${MODE_TEXT}" STREQUAL "${EXPECTED_MODE_TEXT}")
    message(FATAL_ERROR
      "unsupported mode ${UNSUPPORTED_MODE} returned an untyped or wrong reason: ${MODE_TEXT}")
  endif()
endforeach()

set(RECORDED_OUTPUT "${RUN_ROOT}/recorded-replay-observation.txt")
execute_process(
  COMMAND
    "${ORACLE_EXECUTABLE}"
    ${COMMON_ARTIFACT_ARGS}
    --run-dir "${RUN_ROOT}/recorded-replay-run"
    --trace "${TRACE_FILE}"
    --prefix 2
    --output "${RECORDED_OUTPUT}"
    --mode cpu/full
    --subject correct
  RESULT_VARIABLE RECORDED_RESULT
  OUTPUT_VARIABLE RECORDED_STDOUT
  ERROR_VARIABLE RECORDED_STDERR
  TIMEOUT 180
)
if(NOT RECORDED_RESULT EQUAL 0 OR NOT EXISTS "${RECORDED_OUTPUT}")
  message(FATAL_ERROR
    "recorded replay_command failed (${RECORDED_RESULT})\nstdout:\n${RECORDED_STDOUT}\nstderr:\n${RECORDED_STDERR}")
endif()
file(READ "${RECORDED_OUTPUT}" RECORDED_TEXT)
if(NOT "${RECORDED_TEXT}" STREQUAL "${PREFIX_2_REFERENCE}")
  message(FATAL_ERROR "recorded replay_command does not reproduce prefix 2")
endif()

message(STATUS
  "two durable prefixes match production-vs-independent field-by-field across three fresh sessions; wrong-sink rejected; unsupported modes typed")
