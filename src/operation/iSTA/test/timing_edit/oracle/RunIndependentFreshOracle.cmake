# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

foreach(REQUIRED IN ITEMS ORACLE_EXECUTABLE TECH_LEF CELL_LEF LIBERTY VERILOG SDC RUN_ROOT)
  if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
    message(FATAL_ERROR "missing ${REQUIRED}")
  endif()
endforeach()

set(REFERENCE_TEXT "")
foreach(REPEAT RANGE 1 3)
  set(REPEAT_ROOT "${RUN_ROOT}/repeat-${REPEAT}")
  set(OUTPUT_FILE "${REPEAT_ROOT}/observation.txt")
  file(MAKE_DIRECTORY "${REPEAT_ROOT}")
  execute_process(
    COMMAND
      "${ORACLE_EXECUTABLE}"
      --tech-lef "${TECH_LEF}"
      --cell-lef "${CELL_LEF}"
      --liberty "${LIBERTY}"
      --verilog "${VERILOG}"
      --sdc "${SDC}"
      --top stage0_chain
      --run-dir "${REPEAT_ROOT}"
      --output "${OUTPUT_FILE}"
    RESULT_VARIABLE PROBE_RESULT
    OUTPUT_VARIABLE PROBE_STDOUT
    ERROR_VARIABLE PROBE_STDERR
    TIMEOUT 180
  )
  if(NOT PROBE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "fresh oracle repeat ${REPEAT} failed (${PROBE_RESULT})\nstdout:\n${PROBE_STDOUT}\nstderr:\n${PROBE_STDERR}")
  endif()
  if(NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "fresh oracle repeat ${REPEAT} did not write its observation")
  endif()
  file(READ "${OUTPUT_FILE}" OBSERVATION_TEXT)
  foreach(REQUIRED_RECORD IN ITEMS "instance|" "pin|" "net|" "point|" "arc|" "outgoing|" "incoming|" "order|" "electrical_load|" "path|")
    string(FIND "${OBSERVATION_TEXT}" "${REQUIRED_RECORD}" RECORD_OFFSET)
    if(RECORD_OFFSET EQUAL -1)
      message(FATAL_ERROR "fresh oracle observation omits ${REQUIRED_RECORD}")
    endif()
  endforeach()
  if(REPEAT EQUAL 1)
    set(REFERENCE_TEXT "${OBSERVATION_TEXT}")
  elseif(NOT "${OBSERVATION_TEXT}" STREQUAL "${REFERENCE_TEXT}")
    message(FATAL_ERROR "fresh oracle repeat ${REPEAT} differs field-by-field from repeat 1")
  endif()
endforeach()

message(STATUS "three fresh CPU/full semantic-prefix observations match field-by-field")
