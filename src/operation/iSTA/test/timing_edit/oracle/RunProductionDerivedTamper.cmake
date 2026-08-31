# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

foreach(REQUIRED IN ITEMS
        ORACLE_EXECUTABLE PRODUCTION_EXECUTABLE
        TECH_LEF CELL_LEF LIBERTY VERILOG SDC RUN_ROOT)
  if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
    message(FATAL_ERROR "missing ${REQUIRED}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${RUN_ROOT}")
set(TRACE_FILE "${RUN_ROOT}/timing-edit-trace.json")
set(RECEIPT_FILE "${RUN_ROOT}/derived-tamper-receipt.txt")
set(CSR_RECEIPT_FILE "${RECEIPT_FILE}.csr-full.txt")
file(REMOVE "${TRACE_FILE}" "${RECEIPT_FILE}" "${CSR_RECEIPT_FILE}")
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
    "derived-tamper trace generation failed (${TRACE_RESULT})\n"
    "stdout:\n${TRACE_STDOUT}\nstderr:\n${TRACE_STDERR}")
endif()

execute_process(
  COMMAND
    "${PRODUCTION_EXECUTABLE}"
    ${COMMON_ARTIFACT_ARGS}
    --run-dir "${RUN_ROOT}/production-run"
    --trace "${TRACE_FILE}"
    --prefix 1
    --output "${RECEIPT_FILE}"
    --mode cpu/full
    --subject derived-tamper
  RESULT_VARIABLE TAMPER_RESULT
  OUTPUT_VARIABLE TAMPER_STDOUT
  ERROR_VARIABLE TAMPER_STDERR
  TIMEOUT 180
)
if(NOT TAMPER_RESULT EQUAL 0 OR NOT EXISTS "${RECEIPT_FILE}")
  message(FATAL_ERROR
    "production derived-tamper subject failed (${TAMPER_RESULT})\n"
    "stdout:\n${TAMPER_STDOUT}\nstderr:\n${TAMPER_STDERR}")
endif()
if(EXISTS "${CSR_RECEIPT_FILE}")
  message(FATAL_ERROR
    "derived-tamper subject emitted a CSR-full success receipt before publish")
endif()

file(READ "${RECEIPT_FILE}" RECEIPT_TEXT)
string(CONCAT EXPECTED_RECEIPT
    "schema|ista-timing-edit-derived-tamper-receipt/1.0.0\n"
    "derived_tamper_detected|csr_full_exact|prepublish|sealed=false|"
    "published=false|database_generation=0|raw=unchanged|"
    "transaction_generation=0\n")
if(NOT "${RECEIPT_TEXT}" STREQUAL "${EXPECTED_RECEIPT}")
  message(FATAL_ERROR
    "derived-tamper rollback receipt is incomplete: ${RECEIPT_TEXT}")
endif()

message(STATUS
  "native derived tamper rejected before seal/publish; exact generation zero preserved")
