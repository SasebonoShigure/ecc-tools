# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.
cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)
set(STEP8_EXPECTED_CHECKPOINTS pre_poison post_poison post_scatter post_canary post_rerun)
set(STEP8_EXPECTED_POISON
    timing_point_level timing_point_is_clock_point timing_point_scalars timing_point_clock_arrival_maps timing_point_clock_slew_maps
    timing_point_clock_predecessor_maps timing_point_clock_predecessor_arc_delay_maps
    timing_point_clock_predecessor_transition_maps timing_point_data_slew_map timing_point_path_state_map
    arc_scalars arc_transition_maps arc_input_output_delay_map arc_graph_delay_map
    arc_timing_arc_delay_map arc_timing_arc_fallback_provenance database_timing_order)
set(STEP8_EXPECTED_PHASES initialize pre_full full post_full native_initial pre_poison poison
    build candidate compute scatter readback canary rerun release)
set(STEP8_EXPECTED_PUBLICATION
    validate_two_temps remove_probe_sta probe_sta_removed link_oracle link_reference
    unlink_temps validate_lane_inventory validate_two_finals summary_last)
set(STEP8_EXPECTED_CHECKPOINT_BINDINGS pre_poison_json post_poison_json post_scatter_json post_canary_json post_rerun_json)
set(STEP8_EXPECTED_PIPELINE_BINDINGS
    build_result built_variant committed_snapshot candidate_snapshot_input compute_snapshot result_outcome reference_snapshot_result
    scatter_snapshot_result source_raw_bytes source_raw_sha source_candidate_wire source_candidate_sha)
set(STEP8_EXPECTED_PYTHON_SOURCES
    real_differential/real_differential_model.py real_differential/real_differential_wire_contract.py
    real_differential/real_differential_contract.py real_differential/real_differential_controller.py
    real_differential/real_differential_controller_selftest.py reference_input/reference_input_candidate_wire.py
    reference_input/reference_input_recompute_transfer.py)
set(STEP8_EXPECTED_SUMMARY_FLAGS O_RDWR O_CREAT O_EXCL O_NOFOLLOW)
set(STEP8_EXPECTED_CODEC full_owner fixture_forbidden direct_codec enum_only mutation_unsupported
    mutation_prebuild roundtrip_chain_r0 roundtrip_diamond_r0 roundtrip_supported_branches_r0 roundtrip_actual_source roundtrip_distinct matrix_owner)
set(STEP8_EXPECTED_SCHEMA_ACCEPT_KATS full_keyword_bundle type_sensitive_enum_distinct
    boolean_subschema_positions)
set(STEP8_EXPECTED_SCHEMA_REJECTION_KATS
    bad_root_schema empty_root_id nested_schema nested_id title_shape defs_shape defs_child_shape
    properties_shape properties_child_shape type_empty type_unknown type_nonstring type_duplicate
    required_duplicate required_nonstring oneof_empty oneof_nonschema prefixitems_empty
    prefixitems_nonschema items_shape additional_shape pattern_shape pattern_invalid minimum_bool
    maximum_nonfinite minimum_gt_maximum minitems_bool maxitems_negative minitems_gt_maxitems
    const_nonjson const_nonfinite enum_empty enum_duplicate enum_nonfinite ref_shape root_unknown
    nested_unknown required_position properties_position pattern_position minimum_position
    prefixitems_position items_position additional_position minitems_position ref_missing_store
    ref_store_id_drift ref_fragment_missing ref_fragment_nonpointer ref_fragment_escape
    ref_fragment_drift ref_sibling_conjunction ref_cycle public_store_drift)
set(STEP8_EXPECTED_LAUNCH_ZERO_KATS double_flag public_repeats_drift duplicate_nonce)
set(STEP8_EXPECTED_TRANSACTION_KATS
    success invalid validate_two_temps lane-temp-symlink lane-temp-swap lane-temp-reopen
    lane-temp-inode link_oracle link link_reference unlink link-fsync unlink_temps
    validate_two_finals lane-final-symlink lane-final-swap lane-final-reopen lane-final-inode
    reload revalidate summary_last summary-validate summary-symlink summary-swap summary-link-call
    summary-link summary-unlink summary-fsync complete rollback-aggregate remove_probe_sta
    probe_sta_removed sta-missing sta-root-file sta-root-symlink sta-lane-extra
    sta-descendant-symlink sta-rmtree-error validate_lane_inventory
    sta-fsync-error sta-mode-0700 sta-mode-0755 sta-mode-invalid)
set(STEP8_EXPECTED_KATS)
foreach(NAME IN LISTS STEP8_EXPECTED_SCHEMA_ACCEPT_KATS)
  list(APPEND STEP8_EXPECTED_KATS "schema-accept:${NAME}")
endforeach()
foreach(NAME IN LISTS STEP8_EXPECTED_SCHEMA_REJECTION_KATS)
  list(APPEND STEP8_EXPECTED_KATS "schema-reject:${NAME}")
endforeach()
foreach(NAME IN LISTS STEP8_EXPECTED_LAUNCH_ZERO_KATS)
  list(APPEND STEP8_EXPECTED_KATS "launch-zero:${NAME}")
endforeach()
foreach(NAME IN LISTS STEP8_EXPECTED_TRANSACTION_KATS)
  list(APPEND STEP8_EXPECTED_KATS "transaction:${NAME}")
endforeach()
set(STEP8_EXPECTED_MATRIX_ROWS
    "duplicate_json|loader_bytes|parse_strict_json_for_test|_mutate_duplicate_json|duplicate-key" "trailing_json|loader_bytes|parse_strict_json_for_test|_mutate_trailing_json|trailing-data"
    "nan_json|fresh_probe_json_bytes|parse_strict_json_for_test|_mutate_nan_json|non-finite" "pos_inf_json|fresh_probe_json_bytes|parse_strict_json_for_test|_mutate_pos_inf_json|non-finite"
    "neg_inf_json|fresh_probe_json_bytes|parse_strict_json_for_test|_mutate_neg_inf_json|non-finite" "bad_sha|reference_probe|validate_probe|_mutate_bad_sha|bad-sha"
    "bad_pid|reference_probe|validate_probe|_mutate_bad_pid|bad-pid" "bad_schema|reference_probe|validate_probe|_mutate_bad_schema|bad-schema"
    "raw_drift|reference_probe|validate_probe|_mutate_raw_drift|raw-provenance" "hash_drift|reference_probe|validate_probe|_mutate_hash_drift|hash-drift"
    "unsupported|reference_probe|validate_probe|_mutate_unsupported|unsupported" "scatter|reference_probe|validate_probe|_mutate_scatter|scatter"
    "released|reference_probe|validate_probe|_mutate_released|released" "stale_argv|reference_probe|validate_probe|_mutate_stale_argv|stale"
    "bad_arc_key|extended_reference_probe|validate_probe|_mutate_bad_arc_key|ArcKey" "near_tie|extended_reference_probe|validate_probe|_mutate_near_tie|near-tie-actual-numeric"
    "prebuild|reference_probe|validate_probe|_mutate_prebuild|prebuild" "checkpoint_copy|reference_probe|validate_probe|_mutate_checkpoint_copy|checkpoint-copy"
    "canary|reference_pair|validate_pair|_mutate_canary|canary" "tolerance_bool|reference_pair+tolerances|validate_pair|_mutate_tolerance_bool|tolerance"
    "tolerance_nan|reference_pair+tolerances|validate_pair|_mutate_tolerance_nan|tolerance" "tolerance_pos_inf|reference_pair+tolerances|validate_pair|_mutate_tolerance_pos_inf|tolerance"
    "tolerance_neg_inf|reference_pair+tolerances|validate_pair|_mutate_tolerance_neg_inf|tolerance" "tolerance_negative|reference_pair+tolerances|validate_pair|_mutate_tolerance_negative|tolerance"
    "tolerance_signed_zero|reference_pair+tolerances|validate_pair|_mutate_tolerance_signed_zero|tolerance" "tolerance_drift|reference_pair+tolerances|validate_pair|_mutate_tolerance_drift|tolerance"
    "summary_pid|controller_summary|validate_controller_summary|_mutate_summary_pid|summary-pid" "summary_nonce|controller_summary|validate_controller_summary|_mutate_summary_nonce|summary-nonce"
    "summary_repeat_order|controller_summary|validate_controller_summary|_mutate_summary_repeat_order|summary-repeat-order"
    "summary_embedded_schema|controller_summary|validate_controller_summary|_mutate_summary_embedded_schema|summary-embedded-schema")
set(STEP8_EXPECTED_FILES
    CsrRealDifferentialWire.hpp CsrRealDifferentialWire.cpp CsrRealDifferentialProbeSupport.hpp CsrRealDifferentialProbeSupport.cpp
    CsrRealDifferentialScenario.hpp CsrRealDifferentialScenario.cpp CsrRealDifferentialOracleEncoding.hpp CsrRealDifferentialOracleEncoding.cpp
    StaCsrRealOracleProbe.cpp CsrRealDifferentialReferenceEncoding.hpp CsrRealDifferentialReferenceEncoding.cpp CsrRealDifferentialNativeReadback.hpp
    CsrRealDifferentialNativeReadback.cpp CsrRealDifferentialNativeNodeReadback.cpp CsrRealDifferentialNativeArcReadback.cpp StaCsrRealReferenceProbe.cpp CsrRealDifferentialWireContractTest.cpp
    real_differential_model.py real_differential_wire_contract.py real_differential_contract.py real_differential_controller.py real_differential_controller_selftest.py
    manifest.json schema/real_differential_probe_v1.schema.json schema/real_differential_controller_v1.schema.json
    fixtures/supported_branches_cells.lef fixtures/supported_branches.lib fixtures/supported_branches.v
    fixtures/supported_branches.sdc)
function(workflow_assert_exact ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
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
            "Step 8 workflow detector rejected ${DESCRIPTION}: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()
function(workflow_assert_order ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  set(ACTUAL ${${ACTUAL_VARIABLE}})
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "Step 8 workflow detector rejected ${DESCRIPTION} order: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()
function(workflow_check_detector PHASE_VARIABLE CHECKPOINT_VARIABLE POISON_VARIABLE
         PUBLICATION_VARIABLE CHECKPOINT_BINDING_VARIABLE PIPELINE_BINDING_VARIABLE
         REPEATS CASES PAIRS LANES DIRFDS FSYNCS COMMITS MATRIX_VARIABLE FLAGS_VARIABLE
         CODEC_VARIABLE KATS_VARIABLE)
  workflow_assert_order(${PHASE_VARIABLE} STEP8_EXPECTED_PHASES phases)
  workflow_assert_order(${CHECKPOINT_VARIABLE} STEP8_EXPECTED_CHECKPOINTS
                        checkpoints)
  workflow_assert_order(${POISON_VARIABLE} STEP8_EXPECTED_POISON poison)
  workflow_assert_order(${PUBLICATION_VARIABLE} STEP8_EXPECTED_PUBLICATION publication)
  workflow_assert_order(${CHECKPOINT_BINDING_VARIABLE} STEP8_EXPECTED_CHECKPOINT_BINDINGS
                        checkpoint_bindings)
  workflow_assert_order(${PIPELINE_BINDING_VARIABLE} STEP8_EXPECTED_PIPELINE_BINDINGS
                        pipeline_bindings)
  workflow_assert_order(${MATRIX_VARIABLE} STEP8_EXPECTED_MATRIX_ROWS matrix)
  workflow_assert_order(${FLAGS_VARIABLE} STEP8_EXPECTED_SUMMARY_FLAGS summary_flags)
  workflow_assert_order(${CODEC_VARIABLE} STEP8_EXPECTED_CODEC codec)
  workflow_assert_order(${KATS_VARIABLE} STEP8_EXPECTED_KATS selftest_kats)
  math(EXPR EXPECTED_PAIRS "${CASES} * ${REPEATS}")
  math(EXPR EXPECTED_LANES "2 * ${EXPECTED_PAIRS}")
  math(EXPR EXPECTED_DIRFDS "1 + ${EXPECTED_PAIRS} + ${EXPECTED_LANES}")
  if(NOT CASES EQUAL 3 OR REPEATS LESS 3 OR NOT PAIRS EQUAL EXPECTED_PAIRS
     OR NOT LANES EQUAL EXPECTED_LANES OR NOT DIRFDS EQUAL EXPECTED_DIRFDS
     OR NOT FSYNCS EQUAL EXPECTED_DIRFDS OR NOT COMMITS EQUAL 1)
    message(FATAL_ERROR
            "Step 8 workflow detector rejected derived cardinality/transaction: N=${REPEATS} cases=${CASES} pairs=${PAIRS} lanes=${LANES} dirfds=${DIRFDS} fsyncs=${FSYNCS} commits=${COMMITS}")
  endif()
endfunction()
if(DEFINED CSR_STEP8_WORKFLOW_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP8_PHASES_PACKED CSR_STEP8_CHECKPOINTS_PACKED CSR_STEP8_POISON_PACKED
                   CSR_STEP8_PUBLICATION_PACKED CSR_STEP8_CHECKPOINT_BINDINGS_PACKED
                   CSR_STEP8_PIPELINE_BINDINGS_PACKED CSR_STEP8_REPEATS CSR_STEP8_CASES
                   CSR_STEP8_PAIRS CSR_STEP8_LANES CSR_STEP8_DIRFDS CSR_STEP8_FSYNCS
                   CSR_STEP8_COMMITS CSR_STEP8_MATRIX_PACKED CSR_STEP8_SUMMARY_FLAGS_PACKED
                   CSR_STEP8_CODEC_PACKED CSR_STEP8_KATS_PACKED)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  foreach(NAME PHASES CHECKPOINTS POISON PUBLICATION CHECKPOINT_BINDINGS PIPELINE_BINDINGS)
    string(REPLACE "|" ";" DETECTOR_${NAME} "${CSR_STEP8_${NAME}_PACKED}")
  endforeach()
  string(REPLACE "^" ";" DETECTOR_MATRIX "${CSR_STEP8_MATRIX_PACKED}")
  string(REPLACE "|" ";" DETECTOR_FLAGS "${CSR_STEP8_SUMMARY_FLAGS_PACKED}")
  string(REPLACE "|" ";" DETECTOR_CODEC "${CSR_STEP8_CODEC_PACKED}")
  string(REPLACE "|" ";" DETECTOR_KATS "${CSR_STEP8_KATS_PACKED}")
  workflow_check_detector(
      DETECTOR_PHASES DETECTOR_CHECKPOINTS DETECTOR_POISON
      DETECTOR_PUBLICATION DETECTOR_CHECKPOINT_BINDINGS
      DETECTOR_PIPELINE_BINDINGS "${CSR_STEP8_REPEATS}" "${CSR_STEP8_CASES}"
      "${CSR_STEP8_PAIRS}" "${CSR_STEP8_LANES}" "${CSR_STEP8_DIRFDS}"
      "${CSR_STEP8_FSYNCS}" "${CSR_STEP8_COMMITS}" DETECTOR_MATRIX DETECTOR_FLAGS
      DETECTOR_CODEC DETECTOR_KATS)
  return()
endif()
function(workflow_run_detector EXPECT_SUCCESS MUTATION REPEATS)
  set(PHASES ${STEP8_EXPECTED_PHASES})
  set(CHECKPOINTS ${STEP8_EXPECTED_CHECKPOINTS})
  set(POISON ${STEP8_EXPECTED_POISON})
  set(PUBLICATION ${STEP8_EXPECTED_PUBLICATION})
  set(CHECKPOINT_BINDINGS ${STEP8_EXPECTED_CHECKPOINT_BINDINGS})
  set(PIPELINE_BINDINGS ${STEP8_EXPECTED_PIPELINE_BINDINGS})
  set(MATRIX ${STEP8_EXPECTED_MATRIX_ROWS})
  set(FLAGS ${STEP8_EXPECTED_SUMMARY_FLAGS})
  set(CODEC ${STEP8_EXPECTED_CODEC})
  set(KATS ${STEP8_EXPECTED_KATS})
  set(CASES 3)
  math(EXPR PAIRS "${CASES} * ${REPEATS}")
  math(EXPR LANES "2 * ${PAIRS}")
  math(EXPR DIRFDS "1 + ${PAIRS} + ${LANES}")
  set(FSYNCS ${DIRFDS})
  set(COMMITS 1)
  if(MUTATION STREQUAL missing_phase)
    list(REMOVE_ITEM PHASES pre_poison)
  elseif(MUTATION STREQUAL missing_matrix)
    list(REMOVE_AT MATRIX 29)
  elseif(MUTATION STREQUAL reorder_matrix)
    list(POP_FRONT MATRIX FIRST)
    list(APPEND MATRIX "${FIRST}")
  elseif(MUTATION STREQUAL multi_mutation)
    list(POP_FRONT MATRIX FIRST)
    string(REPLACE "|_mutate_duplicate_json|"
                   "|_mutate_duplicate_json+_mutate_trailing_json|" FIRST "${FIRST}")
    list(PREPEND MATRIX "${FIRST}")
  elseif(MUTATION STREQUAL missing_flag)
    list(REMOVE_ITEM FLAGS O_NOFOLLOW)
  elseif(MUTATION STREQUAL reorder_flags)
    set(FLAGS O_CREAT O_RDWR O_EXCL O_NOFOLLOW)
  elseif(MUTATION STREQUAL missing_candidate)
    list(REMOVE_ITEM CODEC full_owner)
  elseif(MUTATION STREQUAL duplicate_full_owner)
    list(APPEND CODEC full_owner)
  elseif(MUTATION STREQUAL fixture_call)
    list(REMOVE_ITEM CODEC fixture_forbidden)
  elseif(MUTATION STREQUAL roundtrip_source)
    list(REMOVE_ITEM CODEC roundtrip_actual_source)
  elseif(MUTATION STREQUAL roundtrip_order)
    list(REMOVE_ITEM CODEC roundtrip_chain_r0 roundtrip_diamond_r0)
    list(INSERT CODEC 6 roundtrip_diamond_r0 roundtrip_chain_r0)
  elseif(MUTATION STREQUAL enum_widen)
    list(REMOVE_ITEM CODEC enum_only)
  elseif(MUTATION STREQUAL missing_schema_kat)
    list(REMOVE_ITEM KATS schema-reject:nested_schema)
  elseif(MUTATION STREQUAL missing_launch_kat)
    list(REMOVE_ITEM KATS launch-zero:duplicate_nonce)
  elseif(MUTATION STREQUAL missing_lane_kat)
    list(REMOVE_ITEM KATS transaction:lane-final-reopen)
  elseif(MUTATION STREQUAL missing_rollback_kat)
    list(REMOVE_ITEM KATS transaction:rollback-aggregate)
  elseif(MUTATION STREQUAL missing_sta_kat)
    list(REMOVE_ITEM KATS transaction:sta-fsync-error)
  elseif(MUTATION STREQUAL missing_sta_mode_kat)
    list(REMOVE_ITEM KATS transaction:sta-mode-invalid)
  elseif(MUTATION STREQUAL missing_publication_cleanup)
    list(REMOVE_ITEM PUBLICATION remove_probe_sta)
  elseif(MUTATION STREQUAL pair)
    math(EXPR PAIRS "${PAIRS} - 1")
  elseif(MUTATION STREQUAL lane OR MUTATION STREQUAL n6_lane)
    math(EXPR LANES "${LANES} - 1")
  elseif(MUTATION MATCHES "^(dirfd|lane_fsync|slot_fsync|root_fsync)$")
    math(EXPR DIRFDS "${DIRFDS} - 1")
    if(MUTATION MATCHES "fsync")
      math(EXPR FSYNCS "${FSYNCS} - 1")
    endif()
  elseif(MUTATION STREQUAL commit)
    set(COMMITS 0)
  endif()
  foreach(NAME PHASES CHECKPOINTS POISON PUBLICATION CHECKPOINT_BINDINGS PIPELINE_BINDINGS FLAGS CODEC KATS)
    string(JOIN "|" ${NAME}_PACKED ${${NAME}})
  endforeach()
  string(JOIN "^" MATRIX_PACKED ${MATRIX})
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -DCSR_STEP8_WORKFLOW_DETECTOR_MODE=TRUE
              "-DCSR_STEP8_PHASES_PACKED=${PHASES_PACKED}"
              "-DCSR_STEP8_CHECKPOINTS_PACKED=${CHECKPOINTS_PACKED}"
              "-DCSR_STEP8_POISON_PACKED=${POISON_PACKED}"
              "-DCSR_STEP8_PUBLICATION_PACKED=${PUBLICATION_PACKED}"
              "-DCSR_STEP8_CHECKPOINT_BINDINGS_PACKED=${CHECKPOINT_BINDINGS_PACKED}"
              "-DCSR_STEP8_PIPELINE_BINDINGS_PACKED=${PIPELINE_BINDINGS_PACKED}"
              "-DCSR_STEP8_REPEATS=${REPEATS}" "-DCSR_STEP8_CASES=${CASES}"
              "-DCSR_STEP8_PAIRS=${PAIRS}" "-DCSR_STEP8_LANES=${LANES}"
              "-DCSR_STEP8_DIRFDS=${DIRFDS}" "-DCSR_STEP8_FSYNCS=${FSYNCS}"
              "-DCSR_STEP8_COMMITS=${COMMITS}" "-DCSR_STEP8_MATRIX_PACKED=${MATRIX_PACKED}"
              "-DCSR_STEP8_SUMMARY_FLAGS_PACKED=${FLAGS_PACKED}"
              "-DCSR_STEP8_CODEC_PACKED=${CODEC_PACKED}"
              "-DCSR_STEP8_KATS_PACKED=${KATS_PACKED}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 8 workflow safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 8 workflow detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 8 workflow poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
  message(STATUS "Step 8 workflow detector receipt: ${MUTATION},N=${REPEATS},exit=${RESULT}")
endfunction()
workflow_run_detector(TRUE safe_n3 3)
workflow_run_detector(TRUE safe_n6 6)
foreach(MUTATION missing_phase missing_matrix reorder_matrix multi_mutation missing_flag
                 reorder_flags missing_candidate duplicate_full_owner fixture_call roundtrip_source
                 roundtrip_order enum_widen missing_schema_kat missing_launch_kat missing_lane_kat
                 missing_rollback_kat missing_sta_kat missing_sta_mode_kat missing_publication_cleanup pair lane dirfd
                 lane_fsync slot_fsync root_fsync commit)
  workflow_run_detector(FALSE "${MUTATION}" 3)
endforeach()
workflow_run_detector(FALSE n6_lane 6)
if(DEFINED CSR_STEP8_WORKFLOW_SELFTEST_ONLY)
  return()
endif()
foreach(REQUIRED ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR PYTHON_EXECUTABLE
                 STEP8_PYTHON_SOURCES_PACKED)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "${REQUIRED} is required")
  endif()
endforeach()
set(STEP8_DIR "${CSR_TEST_ROOT}/real_differential")
function(workflow_read OUTPUT FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 8 workflow file: ${FILE_PATH}")
  endif()
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()
function(workflow_require_hash FILE_PATH EXPECTED)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing frozen Step 8 input: ${FILE_PATH}")
  endif()
  file(SHA256 "${FILE_PATH}" ACTUAL)
  if(NOT ACTUAL STREQUAL EXPECTED)
    message(FATAL_ERROR
            "frozen Step 8 input changed: ${FILE_PATH}: ${ACTUAL}/${EXPECTED}")
  endif()
endfunction()
function(workflow_count CONTENT_VARIABLE TOKEN OUTPUT)
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
function(workflow_require_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
  workflow_count(${CONTENT_VARIABLE} "${TOKEN}" ACTUAL)
  if(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 8 workflow ${DESCRIPTION} count differs: ${ACTUAL}/${EXPECTED}")
  endif()
endfunction()
function(workflow_require_present CONTENT_VARIABLE TOKEN DESCRIPTION)
  string(FIND "${${CONTENT_VARIABLE}}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "Step 8 workflow is missing ${DESCRIPTION}: ${TOKEN}")
  endif()
endfunction()
function(workflow_require_sequence CONTENT_VARIABLE DESCRIPTION)
  set(REMAINDER "${${CONTENT_VARIABLE}}")
  foreach(TOKEN IN LISTS ARGN)
    string(FIND "${REMAINDER}" "${TOKEN}" POSITION)
    if(POSITION EQUAL -1)
      message(FATAL_ERROR
              "Step 8 workflow ${DESCRIPTION} sequence is missing/out of order: ${TOKEN}")
    endif()
    string(LENGTH "${TOKEN}" TOKEN_LENGTH)
    math(EXPR OFFSET "${POSITION} + ${TOKEN_LENGTH}")
    string(SUBSTRING "${REMAINDER}" ${OFFSET} -1 REMAINDER)
  endforeach()
endfunction()
function(workflow_slice_between OUTPUT CONTENT_VARIABLE START_TOKEN END_TOKEN)
  workflow_count(${CONTENT_VARIABLE} "${START_TOKEN}" START_COUNT)
  if(NOT START_COUNT EQUAL 1)
    message(FATAL_ERROR
            "Step 8 workflow slice ${OUTPUT} start marker differs: ${START_COUNT}")
  endif()
  string(FIND "${${CONTENT_VARIABLE}}" "${START_TOKEN}" START_POSITION)
  string(SUBSTRING "${${CONTENT_VARIABLE}}" ${START_POSITION} -1 TAIL)
  string(FIND "${TAIL}" "${END_TOKEN}" END_POSITION)
  if(END_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 8 workflow slice ${OUTPUT} is unordered")
  endif()
  string(SUBSTRING "${TAIL}" 0 ${END_POSITION} SLICE)
  set("${OUTPUT}" "${SLICE}" PARENT_SCOPE)
endfunction()
function(workflow_extract_labeled_sequence OUTPUT CONTENT_VARIABLE DESCRIPTION)
  set(REMAINDER "${${CONTENT_VARIABLE}}")
  set(LABELS)
  foreach(ROW IN LISTS ARGN)
    string(FIND "${ROW}" "|" SEPARATOR)
    if(SEPARATOR LESS 1)
      message(FATAL_ERROR "Step 8 workflow ${DESCRIPTION} has an invalid detector row")
    endif()
    string(SUBSTRING "${ROW}" 0 ${SEPARATOR} LABEL)
    math(EXPR TOKEN_START "${SEPARATOR} + 1")
    string(SUBSTRING "${ROW}" ${TOKEN_START} -1 TOKEN)
    workflow_count(${CONTENT_VARIABLE} "${TOKEN}" TOKEN_COUNT)
    string(FIND "${REMAINDER}" "${TOKEN}" POSITION)
    if(NOT TOKEN_COUNT EQUAL 1 OR POSITION EQUAL -1)
      message(FATAL_ERROR
              "Step 8 workflow ${DESCRIPTION} source token differs: ${LABEL}=${TOKEN_COUNT}")
    endif()
    list(APPEND LABELS "${LABEL}")
    string(LENGTH "${TOKEN}" TOKEN_LENGTH)
    math(EXPR OFFSET "${POSITION} + ${TOKEN_LENGTH}")
    string(SUBSTRING "${REMAINDER}" ${OFFSET} -1 REMAINDER)
  endforeach()
  set("${OUTPUT}" "${LABELS}" PARENT_SCOPE)
endfunction()
set(FROZEN_ROWS
    "${CSR_TEST_ROOT}/baseline/manifest.json|c2cf7aac05719ba7b437b45918cc11ade5fec68f2727135facaa46b7b4f4b47b"
    "${CSR_TEST_ROOT}/baseline/golden/chain.json|a29b1afe4353f34da3c6bbfa6712d58650bd5e85b97884b2df735c18087c74f7"
    "${CSR_TEST_ROOT}/baseline/golden/diamond.json|dfed7bc52f3e5e985127eed3a1a6d76e91649f8704f50188d08a2f0b4b8fea16"
    "${CSR_TEST_ROOT}/projection/projection_default_compat_v1.json|0da3a1b22ffd0ece8198c97854a78e9db342af42c5daad45ebfafb1c9f8fb414"
    "${ISTA_SOURCE_ROOT}/interface/StaExecutionContract.hpp|3985aef36766576a57440a875fcfca2a5e118d2d3b77168e5bdefbb4f1edae1d"
    "${ISTA_SOURCE_ROOT}/interface/StaExecutionContract.cpp|ef437cb0ba026440a0d41579f58162e62f1b9ece343770221a8665f7e32cc514"
    "${CSR_TEST_ROOT}/StaExecutionContractTest.cpp|239e93d25a3e94f882369bd70a5f298f28587b1bcb289f7fc34b89bed746a0fe"
    "${ISTA_SOURCE_ROOT}/benchmark/schema/incremental_sta_samples_v1.schema.json|7ed5a94f536aaa11e7471e082efe3610be44b644782de3e9ddb3d3e28ca44045"
    "${CSR_TEST_ROOT}/projection/StaCsrProjectionProbe.cpp|ce5f3cdc6baf7bbd90ecbf62aab843191d4fde3d7e3b1f7037758a18e08a9bff"
    "${CSR_TEST_ROOT}/csr_session/csr_session_contract.py|0ff13a91b9bb741215f6186ae1c5ce935e920c529ab075c5771a42500862698a"
    "${CSR_TEST_ROOT}/csr_session/csr_session_controller.py|8c095eab9159b372a48d4ed97f4a11ded9ac37122ed3c5ce4a923980512d5fab"
    "${CSR_TEST_ROOT}/csr_session/csr_session_controller_selftest.py|515dc87189027524447d79bc1d459b38c3b5bd64c5f7730578b811c8024916c4")
foreach(ROW IN LISTS FROZEN_ROWS)
  string(REPLACE "|" ";" FIELDS "${ROW}")
  list(GET FIELDS 0 FILE_PATH)
  list(GET FIELDS 1 EXPECTED_HASH)
  workflow_require_hash("${FILE_PATH}" "${EXPECTED_HASH}")
endforeach()
file(GLOB_RECURSE STEP8_FILES RELATIVE "${STEP8_DIR}"
     LIST_DIRECTORIES FALSE "${STEP8_DIR}/*")
list(SORT STEP8_FILES)
set(EXPECTED_FILES ${STEP8_EXPECTED_FILES})
list(SORT EXPECTED_FILES)
if(NOT "${STEP8_FILES}" STREQUAL "${EXPECTED_FILES}")
  message(FATAL_ERROR
          "Step 8 file inventory differs: actual=[${STEP8_FILES}] expected=[${EXPECTED_FILES}]")
endif()
foreach(RELATIVE_FILE IN LISTS STEP8_FILES)
  set(FILE_PATH "${STEP8_DIR}/${RELATIVE_FILE}")
  if(RELATIVE_FILE MATCHES "[.](cpp|hpp|py|json)$")
    workflow_read(FILE_CONTENT "${FILE_PATH}")
    string(REGEX MATCHALL "\n" NEWLINES "${FILE_CONTENT}")
    list(LENGTH NEWLINES LINE_COUNT)
    string(REGEX MATCH "\n$" TRAILING_NEWLINE "${FILE_CONTENT}")
    if(TRAILING_NEWLINE STREQUAL "")
      math(EXPR LINE_COUNT "${LINE_COUNT} + 1")
    endif()
    set(LIMIT 700)
    if(RELATIVE_FILE MATCHES "StaCsrReal.*Probe[.]cpp$|real_differential_controller(_selftest)?[.]py$")
      set(LIMIT 550)
    elseif(RELATIVE_FILE MATCHES "(Encoding|Readback|Scenario)[.](cpp|hpp)$")
      set(LIMIT 500)
    endif()
    if(NOT LINE_COUNT LESS LIMIT)
      message(STATUS
              "Step 8 advisory line target exceeded: ${RELATIVE_FILE}=${LINE_COUNT}/${LIMIT}")
    endif()
  endif()
  if(RELATIVE_FILE MATCHES "[.](cpp|hpp|py)$")
    string(SUBSTRING "${FILE_CONTENT}" 0 500 HEADER)
    string(FIND "${HEADER}" "Mulan PSL v2" LICENSE)
    if(LICENSE EQUAL -1)
      message(FATAL_ERROR "Step 8 Mulan header missing: ${RELATIVE_FILE}")
    endif()
  endif()
  if(RELATIVE_FILE MATCHES "[.](cpp|hpp)$")
    foreach(FORBIDDEN const_cast "#include \".*[.]cpp\"" -ffast-math -Ofast)
      string(REGEX MATCH "${FORBIDDEN}" MATCHED "${FILE_CONTENT}")
      if(MATCHED)
        message(FATAL_ERROR
                "Step 8 C++ hygiene violation: ${RELATIVE_FILE}: ${FORBIDDEN}")
      endif()
    endforeach()
  endif()
endforeach()
workflow_read(MODEL "${STEP8_DIR}/real_differential_model.py")
workflow_read(WIRE "${STEP8_DIR}/real_differential_wire_contract.py")
workflow_read(CONTRACT "${STEP8_DIR}/real_differential_contract.py")
workflow_read(CONTROLLER "${STEP8_DIR}/real_differential_controller.py")
workflow_read(SELFTEST "${STEP8_DIR}/real_differential_controller_selftest.py")
workflow_read(CANDIDATE "${CSR_TEST_ROOT}/reference_input/reference_input_candidate_wire.py")
workflow_read(FIXTURE "${CSR_TEST_ROOT}/reference_input/reference_input_wire_fixture.py")
workflow_read(TRANSFER "${CSR_TEST_ROOT}/reference_input/reference_input_recompute_transfer.py")
workflow_read(MANIFEST "${STEP8_DIR}/manifest.json")
workflow_read(BASELINE_MANIFEST "${CSR_TEST_ROOT}/baseline/manifest.json")
string(JSON BASELINE_CASE0 GET "${BASELINE_MANIFEST}" cases 0 name)
string(JSON BASELINE_CASE1 GET "${BASELINE_MANIFEST}" cases 1 name)
string(JSON EXTENDED_CASE0 GET "${MANIFEST}" extended_cases 0 name)
set(STEP8_EXPECTED_ROUNDTRIP_CASES chain diamond supported_branches)
set(STEP8_ACTUAL_ROUNDTRIP_CASES "${BASELINE_CASE0}" "${BASELINE_CASE1}" "${EXTENDED_CASE0}")
workflow_assert_order(STEP8_ACTUAL_ROUNDTRIP_CASES STEP8_EXPECTED_ROUNDTRIP_CASES roundtrip_cases)
workflow_read(POISON_HEADER "${CSR_TEST_ROOT}/CsrReferencePoisonSupport.hpp")
workflow_read(STEP8_CMAKE "${CSR_TEST_ROOT}/CsrRealDifferentialTests.cmake")
workflow_read(RECOMPUTE_MODEL "${CSR_TEST_ROOT}/reference_input/reference_input_recompute_model.py")
string(REPLACE "|" ";" STEP8_ACTUAL_PYTHON_SOURCES "${STEP8_PYTHON_SOURCES_PACKED}")
workflow_assert_order(STEP8_ACTUAL_PYTHON_SOURCES STEP8_EXPECTED_PYTHON_SOURCES python_sources)
foreach(SOURCE IN LISTS STEP8_ACTUAL_PYTHON_SOURCES)
  set(PYTHON_PATH "${CSR_TEST_ROOT}/${SOURCE}")
  workflow_read(PYTHON_CONTENT "${PYTHON_PATH}")
  string(SUBSTRING "${PYTHON_CONTENT}" 0 500 PYTHON_HEADER)
  string(FIND "${PYTHON_HEADER}" "Mulan PSL v2" MULAN_POSITION)
  if(MULAN_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 8 exact-seven Mulan header missing: ${SOURCE}")
  endif()
  foreach(FORBIDDEN "# noqa" "# fmt: off" "# fmt: skip" "fmt:off" "fmt:skip"
                    "extend-exclude")
    string(FIND "${PYTHON_CONTENT}" "${FORBIDDEN}" FORBIDDEN_POSITION)
    if(NOT FORBIDDEN_POSITION EQUAL -1)
      message(FATAL_ERROR "Step 8 exact-seven Python hygiene violation: ${SOURCE}: ${FORBIDDEN}")
    endif()
  endforeach()
  string(FIND "${PYTHON_CONTENT}" "\r" CR_POSITION)
  if(NOT CR_POSITION EQUAL -1)
    message(FATAL_ERROR "Step 8 exact-seven Python contains CR bytes: ${SOURCE}")
  endif()
  string(REGEX MATCHALL "\n" PYTHON_NEWLINES "${PYTHON_CONTENT}")
  list(LENGTH PYTHON_NEWLINES PYTHON_LINES)
  if(NOT PYTHON_LINES LESS 700)
    message(STATUS "Step 8 exact-seven advisory line target exceeded: ${SOURCE}=${PYTHON_LINES}/700")
  endif()
endforeach()
foreach(TOKEN "3e112e0be826d695" "3eb0c6f7a0b5ed8d" "ensure_ascii=False" "separators=(\",\", \":\")" "allow_nan=False")
  workflow_require_present(MODEL "${TOKEN}" "canonical manifest token")
endforeach()
foreach(TOKEN IN LISTS STEP8_EXPECTED_CHECKPOINTS STEP8_EXPECTED_POISON)
  workflow_require_count(MODEL "\"${TOKEN}\"" 1 "model workflow token ${TOKEN}")
endforeach()
workflow_slice_between(POISON_BLOCK POISON_HEADER "kReferencePoisonFamilyTokens = {" "};")
string(REGEX MATCHALL "\"[a-z_]+\"" POISON_MATCHES "${POISON_BLOCK}")
set(STEP8_ACTUAL_POISON)
foreach(MATCH IN LISTS POISON_MATCHES)
  string(REGEX REPLACE "^\"|\"$" "" TOKEN "${MATCH}")
  list(APPEND STEP8_ACTUAL_POISON "${TOKEN}")
endforeach()
workflow_require_count(CONTRACT "transfer_oracle.recompute_transfer(" 0 "duplicate direct transfer recomputation")
workflow_require_count(CONTRACT "reference_input_recompute_transfer" 0 "contract direct transfer import")
workflow_require_sequence(RECOMPUTE_MODEL "model-owned transfer"
    "def recompute_model(" "transfer = transfer_oracle.recompute_transfer(" "return ModelFacts(")
foreach(TOKEN "owner" "ports" "signature" "candidate")
  workflow_require_present(TRANSFER "${TOKEN}" "transfer unique owner token")
endforeach()
set(WORKFLOW_AST [=[
import ast, pathlib, re, sys
paths = [pathlib.Path(p) for p in sys.argv[1:9]]
ctext, stext, ktext, xtext, mtext, wtext, ttext, ftext = (p.read_text(encoding="utf-8") for p in paths)
ctree, stree, ktree, xtree, mtree, wtree, ttree, ftree = (ast.parse(t) for t in (ctext, stext, ktext, xtext, mtext, wtext, ttext, ftext))
step8_trees = (mtree, wtree, xtree, ctree, stree, ktree, ttree)
def fail(message): raise SystemExit("Step 8 workflow AST rejected " + message)
def name(node):
    if isinstance(node, ast.Name): return node.id
    if isinstance(node, ast.Attribute): return name(node.value) + "." + node.attr
    if isinstance(node, ast.Constant) and isinstance(node.value, str): return node.value
    fail("nonliteral matrix field")
def defs(tree, wanted): return [n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == wanted]
def call_nodes(tree, wanted): return [n for n in ast.walk(tree) if isinstance(n, ast.Call) and ast.unparse(n.func) == wanted]
def literal_tuple(tree, wanted):
    values = []
    for node in tree.body:
        target = node.targets[0] if isinstance(node, ast.Assign) and len(node.targets) == 1 else None
        if isinstance(target, ast.Name) and target.id == wanted: values.append(ast.literal_eval(node.value))
    if len(values) != 1 or not isinstance(values[0], tuple): fail("literal tuple " + wanted)
    if not values[0] or any(not isinstance(item, str) for item in values[0]) or len(values[0]) != len(set(values[0])): fail("literal tuple values " + wanted)
    return values[0]
schema_accept = literal_tuple(stree, "_SCHEMA_ACCEPT_KATS")
schema_reject = literal_tuple(stree, "_SCHEMA_REJECTION_KATS")
launch_zero = literal_tuple(stree, "_LAUNCH_ZERO_KATS")
transaction_modes = literal_tuple(stree, "_TRANSACTION_MODES")
schema_tests, launch_tests, transaction_tests = (defs(stree, value) for value in ("_schema_tests", "_launch_zero_tests", "_transaction_test"))
if any(len(nodes) != 1 for nodes in (schema_tests, launch_tests, transaction_tests)): fail("selftest KAT owners")
schema_source = ast.unparse(schema_tests[0])
if not all(token in schema_source for token in ("_SCHEMA_ACCEPT_KATS", "_SCHEMA_REJECTION_KATS", "validate_schema_for_test")): fail("schema KAT binding")
launch_source = ast.unparse(launch_tests[0])
if len(call_nodes(launch_tests[0], "subprocess.run")) != 1 or len(call_nodes(launch_tests[0], "api.prepare_layout")) != 1 or call_nodes(launch_tests[0], "api.spawn_and_drain"): fail("launch-zero real process/API binding")
if not all(token in launch_source for token in ("--self-test", "--public-negative-self-test", "public_repeats_drift", "duplicate_nonce", "PYTHONDONTWRITEBYTECODE")): fail("launch-zero flags/repeat/nonce KAT")
transaction_source = ast.unparse(transaction_tests[0])
lane_tokens = ("lane-temp-symlink", "lane-temp-swap", "lane-temp-reopen", "lane-temp-inode", "lane-final-symlink", "lane-final-swap", "lane-final-reopen", "lane-final-inode")
if not set(lane_tokens).issubset(transaction_modes): fail("lane transaction mode inventory")
if not all(token in transaction_source for token in ("poison_lane", "rollback_targets * 2", "rollback-aggregate", "aggregate unlink", "aggregate fsync")): fail("lane/rollback KAT binding")
sta_tokens = ("remove_probe_sta", "probe_sta_removed", "sta-missing", "sta-root-file", "sta-root-symlink", "sta-lane-extra", "sta-descendant-symlink", "sta-rmtree-error", "validate_lane_inventory", "sta-fsync-error", "sta-mode-0700", "sta-mode-0755", "sta-mode-invalid")
if not set(sta_tokens).issubset(transaction_modes): fail("STA transaction mode inventory")
if not all(token in transaction_source for token in ("sta_removals", "rmtree_attempts", "sta_fsync_poisoned", "external sta target changed", "commit success lane inventory differs", "transaction sta rmtree call differs", "sta fsync failure receipt differs")): fail("STA cleanup KAT binding")
if "directory.chmod(0o700)" not in stext or "report.chmod(0o600)" not in stext: fail("STA umask-077 fixture binding")
stub_values = [n.value.value for n in stree.body if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name) and n.targets[0].id == "_STUB" and isinstance(n.value, ast.Constant) and isinstance(n.value.value, str)]
if len(stub_values) != 1: fail("probe stub owner")
stub_tree = ast.parse(stub_values[0])
requested_mode = [n for n in ast.walk(stub_tree) if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name) and n.targets[0].id == "requested_mode" and ast.unparse(n.value) == "int(permissions, 8)"]
stub_opens = [n for n in ast.walk(stub_tree) if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name) and n.targets[0].id == "fd" and isinstance(n.value, ast.Call) and ast.unparse(n.value.func) == "os.open"]
stub_fchmods = call_nodes(stub_tree, "os.fchmod")
stub_writes = [n for n in call_nodes(stub_tree, "os.write") if n.args and ast.unparse(n.args[0]) == "fd"]
if (
    len(requested_mode) != 1
    or len(stub_opens) != 1
    or len(stub_opens[0].value.args) != 3
    or ast.unparse(stub_opens[0].value.args[2]) != "requested_mode"
    or len(stub_fchmods) != 1
    or len(stub_fchmods[0].args) != 2
    or [ast.unparse(arg) for arg in stub_fchmods[0].args] != ["fd", "requested_mode"]
    or len(stub_writes) != 1
    or not (
        requested_mode[0].lineno
        < stub_opens[0].lineno
        < stub_fchmods[0].lineno
        < stub_writes[0].lineno
    )
): fail("probe stub exact temp mode binding")
kats = [*("schema-accept:" + item for item in schema_accept), *("schema-reject:" + item for item in schema_reject), *("launch-zero:" + item for item in launch_zero), *("transaction:" + item for item in transaction_modes)]
cdefs = [n for n in ast.walk(ctree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
commit = defs(ctree, "commit_validated_run")
commit_calls = [n for n in ast.walk(ctree) if isinstance(n, ast.Call) and name(n.func).endswith("commit_validated_run")]
if len(commit) != 1 or len(commit_calls) != 1: fail("commit owner/call")
probe_cleanup = defs(ctree, "_remove_probe_sta")
probe_cleanup_calls = call_nodes(commit[0], "_remove_probe_sta")
if len(probe_cleanup) != 1 or len(probe_cleanup_calls) != 1: fail("probe STA cleanup owner/call")
probe_cleanup_source = ast.unparse(probe_cleanup[0])
rmtree_calls = call_nodes(probe_cleanup[0], "shutil.rmtree")
if (
    len(rmtree_calls) != 1
    or len(rmtree_calls[0].args) != 1
    or not isinstance(rmtree_calls[0].args[0], ast.Constant)
    or rmtree_calls[0].args[0].value != "sta"
    or len(rmtree_calls[0].keywords) != 1
    or rmtree_calls[0].keywords[0].arg != "dir_fd"
    or ast.unparse(rmtree_calls[0].keywords[0].value) != "lane_fd"
): fail("probe STA fd-relative rmtree")
if not all(token in probe_cleanup_source for token in (
    "shutil.rmtree.avoids_symlink_attacks", "follow_symlinks=False",
    "contract.file_identity", "os.fsync(lane_fd)", "_lane_inventory(lane_fd)",
)): fail("probe STA cleanup safety binding")
sta_mode_sets = [set(n.value for n in node.elts if isinstance(n, ast.Constant) and type(n.value) is int) for node in ast.walk(probe_cleanup[0]) if isinstance(node, ast.Set)]
if sta_mode_sets.count({0o700, 0o755}) != 1: fail("probe STA accepted mode set")
def direct_call(statement, wanted):
    return statement.value if isinstance(statement, ast.Expr) and isinstance(statement.value, ast.Call) and ast.unparse(statement.value.func) == wanted else None
def assigned(statement, wanted):
    return statement.value if isinstance(statement, ast.Assign) and len(statement.targets) == 1 and isinstance(statement.targets[0], ast.Name) and statement.targets[0].id == wanted else None
def tuple_names(node):
    return [item.id for item in node.elts] if isinstance(node, ast.Tuple) and all(isinstance(item, ast.Name) for item in node.elts) else []
def source(node): return "" if node is None else ast.unparse(node)
cleanup_body = probe_cleanup[0].body
prepared_defs = [node for node in cleanup_body if assigned(node, "prepared") is not None]
phase_loops = [node for node in cleanup_body if isinstance(node, ast.For)]
if (
    len(cleanup_body) != 4 or len(prepared_defs) != 1
    or prepared_defs[0] is not cleanup_body[1]
    or not isinstance(prepared_defs[0].value, ast.List) or prepared_defs[0].value.elts
    or len(phase_loops) != 2 or cleanup_body[2:] != phase_loops
    or tuple_names(phase_loops[0].target) != ["lane", "lane_fd", "file_fd", "saved"]
    or source(phase_loops[0].iter) != "temps"
    or tuple_names(phase_loops[1].target) != ["lane", "lane_fd", "file_fd", "saved", "lane_identity", "sta_identity"]
    or source(phase_loops[1].iter) != "prepared"
): fail("probe STA two-phase layout")
phase_a, phase_b = phase_loops
if len(phase_a.body) != 9: fail("probe STA Phase A shape")
phase_a_bind = direct_call(phase_a.body[0], "_assert_linked")
phase_a_append = direct_call(phase_a.body[8], "prepared.append")
bind_args = ["file_fd", "lane_fd", "lane.temporary_result.name", "saved", "1", "'lane temp'"]
if (
    phase_a_bind is None or phase_a_bind.keywords
    or [source(arg) for arg in phase_a_bind.args] != bind_args
    or source(assigned(phase_a.body[1], "lane_info")) != "os.fstat(lane_fd)"
    or source(assigned(phase_a.body[2], "lane_identity")) != "contract.file_identity(lane_info)"
    or direct_call(phase_a.body[3], "ensure") is None
    or direct_call(phase_a.body[4], "ensure") is None
    or [source(node) for node in call_nodes(phase_a.body[4], "_lane_inventory")] != ["_lane_inventory(lane_fd)"]
    or source(assigned(phase_a.body[5], "sta_info")) != "os.stat('sta', dir_fd=lane_fd, follow_symlinks=False)"
    or source(assigned(phase_a.body[6], "sta_identity")) != "contract.file_identity(sta_info)"
    or direct_call(phase_a.body[7], "ensure") is None
    or phase_a_append is None or phase_a_append.keywords or len(phase_a_append.args) != 1
    or source(phase_a_append.args[0]) != "(lane, lane_fd, file_fd, saved, lane_identity, sta_identity)"
): fail("probe STA Phase A preparation")
if len(phase_b.body) != 8: fail("probe STA Phase B shape")
lane_recheck = {source(node) for node in ast.walk(phase_b.body[0]) if isinstance(node, ast.Compare)}
sta_recheck = {source(node) for node in ast.walk(phase_b.body[2]) if isinstance(node, ast.Compare)}
pre_bind = direct_call(phase_b.body[3], "_assert_linked")
remove_call = direct_call(phase_b.body[4], "shutil.rmtree")
fsync_call = direct_call(phase_b.body[5], "os.fsync")
post_bind = direct_call(phase_b.body[7], "_assert_linked")
if (
    direct_call(phase_b.body[0], "ensure") is None
    or lane_recheck != {"contract.file_identity(os.fstat(lane_fd)) == lane_identity"}
    or source(assigned(phase_b.body[1], "current_sta")) != "os.stat('sta', dir_fd=lane_fd, follow_symlinks=False)"
    or direct_call(phase_b.body[2], "ensure") is None
    or sta_recheck != {"contract.file_identity(current_sta) == sta_identity"}
    or pre_bind is None or post_bind is None or pre_bind.keywords or post_bind.keywords
    or [source(arg) for arg in pre_bind.args] != bind_args
    or [source(arg) for arg in post_bind.args] != bind_args
    or remove_call is not rmtree_calls[0]
    or fsync_call is None or fsync_call.keywords or [source(arg) for arg in fsync_call.args] != ["lane_fd"]
    or direct_call(phase_b.body[6], "ensure") is None
    or [source(node) for node in call_nodes(phase_b.body[6], "_lane_inventory")] != ["_lane_inventory(lane_fd)"]
): fail("probe STA Phase B identity/removal/rebind order")
hardlink_calls = sorted(call_nodes(commit[0], "_hardlink"), key=lambda node: node.lineno)
literal_phases = {value: [node for node in call_nodes(commit[0], "phase") if len(node.args) == 1 and not node.keywords and isinstance(node.args[0], ast.Constant) and node.args[0].value == value] for value in ("validate_two_temps", "remove_probe_sta", "probe_sta_removed")}
if (
    len(hardlink_calls) != 2 or any(len(nodes) != 1 for nodes in literal_phases.values())
    or not (literal_phases["validate_two_temps"][0].lineno < literal_phases["remove_probe_sta"][0].lineno < probe_cleanup_calls[0].lineno < literal_phases["probe_sta_removed"][0].lineno < hardlink_calls[0].lineno)
): fail("probe STA cleanup before first hardlink")
rollback_loops = [n for n in ast.walk(commit[0]) if isinstance(n, ast.For) and isinstance(n.iter, ast.Call) and ast.unparse(n.iter.func) == "range" and len(n.iter.args) == 1 and isinstance(n.iter.args[0], ast.Constant) and n.iter.args[0].value == 2]
if len(rollback_loops) != 1 or len(call_nodes(rollback_loops[0], "_unlink")) != 1: fail("rollback exact two rounds")
commit_source = ast.unparse(commit[0])
if not all(token in commit_source for token in ("lane.final_result.name", "temp_name", "final_name", "rollback fsync", "cleanup:")): fail("rollback all targets/error aggregation")
summary_calls = [n for n in ast.walk(ctree) if isinstance(n, ast.Call) and name(n.func).endswith("validate_controller_summary")]
if len(summary_calls) < 2 or any(len(n.args) != 7 or n.keywords for n in summary_calls): fail("summary seven arguments")
assigns = {}
for node in ast.walk(ctree):
    if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name): assigns[node.targets[0].id] = node.value
def symbols(node): return {name(n) for n in ast.walk(node) if isinstance(n, (ast.Name, ast.Attribute))}
pair, lane = assigns.get("pair_count"), assigns.get("lane_count")
if not isinstance(pair, ast.BinOp) or not isinstance(pair.op, ast.Mult) or "repeats" not in symbols(pair): fail("pair_count formula")
if not isinstance(lane, ast.BinOp) or not isinstance(lane.op, ast.Mult) or "pair_count" not in symbols(lane) or 2 not in [n.value for n in ast.walk(lane) if isinstance(n, ast.Constant)]: fail("lane_count formula")
def flag_parts(node):
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr): return flag_parts(node.left) + flag_parts(node.right)
    return [name(node).split(".")[-1]]
opens = [n for n in ast.walk(ctree) if isinstance(n, ast.Call) and name(n.func) == "os.open" and len(n.args) > 1]
summary_flags = [flag_parts(n.args[1]) for n in opens if "O_RDWR" in flag_parts(n.args[1])]
if len(summary_flags) != 1: fail("summary writer owner")
fsync_helpers = []
for function in cdefs:
    calls = sorted((n for n in ast.walk(function) if isinstance(n, ast.Call) and name(n.func) == "os.fsync"), key=lambda n: n.lineno); args = [ast.get_source_segment(ctext, n.args[0]).lower() for n in calls if n.args]
    if len(args) == 3 and "lane" in args[0] and "slot" in args[1] and "root" in args[2]: fsync_helpers.append(function.name)
if len(fsync_helpers) != 1: fail("lane-slot-root fsync helper")
if sum(1 for n in ast.walk(ctree) if isinstance(n, ast.Call) and name(n.func).endswith(fsync_helpers[0])) < 4: fail("fsync lifecycle calls")
if not {6, 18, 36, 55}.issubset({n.value for n in ast.walk(stree) if isinstance(n, ast.Constant) and type(n.value) is int}): fail("N6 receipt")
root = pathlib.Path(sys.argv[9]).resolve()
owners = [p.resolve() for p in root.rglob("*.py") if not {".git", ".venv", "__pycache__"}.intersection(p.parts) and not any(x.startswith("build") for x in p.parts) and re.search(r"(?m)^def encode_full_candidate_wire\(", p.read_text(encoding="utf-8"))]
if owners != [paths[2].resolve()]: fail("full codec owner")
step8_texts = (mtext, wtext, xtext, ctext, stext, ktext, ttext)
if ftext.count("def encode_candidate_wire(") != 1 or any("reference_input_wire_fixture" in t or re.search(r"(?<!full_)encode_candidate_wire\s*\(", t) for t in step8_texts): fail("fixture encoder import/call")
def edges(module): return [tuple((a.name, a.asname) for a in n.names) for n in ast.walk(stree) if isinstance(n, ast.ImportFrom) and n.module == module]
if edges("reference_input_candidate_wire") != [(("decode_candidate_wire", None), ("encode_full_candidate_wire", None))] or edges("reference_input_model") != [(("UnsupportedReason", None),)] or any(isinstance(n, ast.Import) and any(a.name == "reference_input_model" for a in n.names) for n in ast.walk(stree)): fail("direct codec/enum imports")
enum_uses = [ast.unparse(n) for n in ast.walk(stree) if isinstance(n, ast.Attribute) and ast.unparse(n).startswith("UnsupportedReason.")]
if not enum_uses or set(enum_uses) != {"UnsupportedReason.PARASITIC_NET"}: fail("enum-only access")
if len(call_nodes(xtree, "candidate_wire.decode_candidate_wire")) != 1 or [len(call_nodes(stree, n)) for n in ("decode_candidate_wire", "encode_full_candidate_wire")] != [2, 2] or any(call_nodes(t, n) for t in (ctree, mtree, wtree, xtree, ttree, ktree) for n in ("decode_candidate_wire", "encode_full_candidate_wire")): fail("direct codec calls")
wanted = ("_reencode_candidate_mutation", "_mutate_unsupported", "_mutate_prebuild", "_validate_candidate_roundtrips", "run_public_negative_matrix")
owned = [defs(stree, n) for n in wanted]
if any(len(nodes) != 1 for nodes in owned): fail("codec helper owner")
reencode, unsupported, prebuild, roundtrip, public_matrix = (nodes[0] for nodes in owned)
case_nodes = [n.value for n in stree.body if isinstance(n, (ast.Assign, ast.AnnAssign)) and isinstance(n.target if isinstance(n, ast.AnnAssign) else n.targets[0] if len(n.targets) == 1 else None, ast.Name) and (n.target if isinstance(n, ast.AnnAssign) else n.targets[0]).id == "_CANDIDATE_ROUNDTRIP_CASES"]
if len(case_nodes) != 1 or ast.literal_eval(case_nodes[0]) != ("chain", "diamond", "supported_branches"): fail("roundtrip cases")
h_calls = {ast.unparse(n.func) for n in ast.walk(reencode) if isinstance(n, ast.Call)}; h_constants = {n.value for n in ast.walk(reencode) if isinstance(n, ast.Constant)}
if not {"encode_full_candidate_wire", "decode_candidate_wire", "hashlib.sha256"}.issubset(h_calls) or not {"snapshot_candidate_wire_hex", "snapshot_candidate_sha256"}.issubset(h_constants) or sum(isinstance(n, ast.Compare) for n in ast.walk(reencode)) < 2: fail("mutation reencode/SHA/redecode/deep-exact")
u_source, p_source = ast.unparse(unsupported), ast.unparse(prebuild)
if "UnsupportedReason.PARASITIC_NET" not in u_source or "unsupported_reasons" not in u_source or not any(isinstance(n, ast.Tuple) and not n.elts for n in ast.walk(unsupported)): fail("unsupported mutation")
if "session_token" not in p_source or not any(isinstance(n, ast.Constant) and type(n.value) is int and n.value == 0 for n in ast.walk(prebuild)) or not any(isinstance(n, ast.Compare) for n in ast.walk(prebuild)): fail("prebuild mutation")
if any(not {"dataclasses.replace", "_reencode_candidate_mutation"}.issubset({ast.unparse(n.func) for n in ast.walk(fn) if isinstance(n, ast.Call)}) for fn in (unsupported, prebuild)) or len(call_nodes(stree, "_reencode_candidate_mutation")) != 2: fail("candidate mutation construction")
r_source = ast.unparse(roundtrip); r_calls = {ast.unparse(n.func) for n in ast.walk(roundtrip) if isinstance(n, ast.Call)}
r_constants = {n.value for n in ast.walk(roundtrip) if isinstance(n, ast.Constant)}
if "records[case_index * repeats][1]" not in r_source or not {"bytes.fromhex", "hashlib.sha256", "decode_candidate_wire", "encode_full_candidate_wire", "set", "len"}.issubset(r_calls) or not {"snapshot_candidate_wire_hex", "snapshot_candidate_sha256"}.issubset(r_constants) or not any(n.endswith(".add") for n in r_calls) or sum(isinstance(n, ast.Compare) for n in ast.walk(roundtrip)) < 3: fail("actual repeat0 distinct roundtrip")
if len(call_nodes(stree, "_validate_candidate_roundtrips")) != 1 or len(call_nodes(public_matrix, "_validate_candidate_roundtrips")) != 1: fail("fresh matrix roundtrip owner")
codec = ["full_owner", "fixture_forbidden", "direct_codec", "enum_only", "mutation_unsupported", "mutation_prebuild", "roundtrip_chain_r0", "roundtrip_diamond_r0", "roundtrip_supported_branches_r0", "roundtrip_actual_source", "roundtrip_distinct"]
matrix_nodes = []
for node in stree.body:
    target = node.target if isinstance(node, ast.AnnAssign) else node.targets[0] if isinstance(node, ast.Assign) and len(node.targets) == 1 else None
    if isinstance(target, ast.Name) and target.id == "PUBLIC_NEGATIVE_MATRIX": matrix_nodes.append(node.value)
if len(matrix_nodes) != 1 or not isinstance(matrix_nodes[0], (ast.List, ast.Tuple)): fail("matrix owner")
rows = []
for row in matrix_nodes[0].elts:
    fields = row.args if isinstance(row, ast.Call) else row.elts if isinstance(row, ast.Tuple) else []
    if len(fields) != 5 or not isinstance(fields[3], (ast.Name, ast.Attribute)): fail("matrix five fields/single mutation")
    values = [name(fields[i]).split(".")[-1] for i in (0, 1, 2, 3, 4)]
    if len(defs(stree, values[3])) != 1: fail("matrix mutation owner " + values[3])
    rows.append("|".join(values))
codec.append("matrix_owner")
publication = sorted((n for n in ast.walk(commit[0]) if isinstance(n, ast.Constant) and n.value in {"validate_two_temps","remove_probe_sta","probe_sta_removed","link_oracle","link_reference","unlink_temps","validate_lane_inventory","validate_two_finals","summary_last"}), key=lambda n: n.lineno)
print("MATRIX=" + "^".join(rows)); print("FLAGS=" + "|".join(summary_flags[0])); print("CODEC=" + "|".join(codec)); print("KATS=" + "|".join(kats))
print("PUBLICATION=" + "|".join(n.value for n in publication)); print("COMMIT=1\nFSYNC=1|1|1")
]=])
execute_process(COMMAND "${PYTHON_EXECUTABLE}" -c "${WORKFLOW_AST}"
                "${STEP8_DIR}/real_differential_controller.py" "${STEP8_DIR}/real_differential_controller_selftest.py"
                "${CSR_TEST_ROOT}/reference_input/reference_input_candidate_wire.py" "${STEP8_DIR}/real_differential_contract.py"
                "${STEP8_DIR}/real_differential_model.py" "${STEP8_DIR}/real_differential_wire_contract.py"
                "${CSR_TEST_ROOT}/reference_input/reference_input_recompute_transfer.py" "${CSR_TEST_ROOT}/reference_input/reference_input_wire_fixture.py" "${CSR_TEST_ROOT}/../../../../../../.."
                RESULT_VARIABLE AST_RESULT OUTPUT_VARIABLE AST_OUT ERROR_VARIABLE AST_ERR)
if(NOT AST_RESULT EQUAL 0)
  message(FATAL_ERROR "Step 8 workflow AST contract failed: ${AST_OUT}${AST_ERR}")
endif()
foreach(FIELD MATRIX FLAGS CODEC KATS PUBLICATION COMMIT FSYNC)
  string(REGEX MATCH "${FIELD}=([^\n]+)" FIELD_MATCH "${AST_OUT}")
  if(FIELD_MATCH STREQUAL "")
    message(FATAL_ERROR "Step 8 workflow AST receipt missing ${FIELD}")
  endif()
  set(STEP8_ACTUAL_${FIELD}_PACKED "${CMAKE_MATCH_1}")
endforeach()
string(REPLACE "^" ";" STEP8_ACTUAL_MATRIX_ROWS "${STEP8_ACTUAL_MATRIX_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_SUMMARY_FLAGS "${STEP8_ACTUAL_FLAGS_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_PUBLICATION "${STEP8_ACTUAL_PUBLICATION_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_CODEC "${STEP8_ACTUAL_CODEC_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_KATS "${STEP8_ACTUAL_KATS_PACKED}")
string(JSON STEP8_EXTENDED_CASES LENGTH "${MANIFEST}" extended_cases)
string(JSON STEP8_BASELINE_CASES LENGTH "${BASELINE_MANIFEST}" cases)
math(EXPR STEP8_ACTUAL_CASES "${STEP8_EXTENDED_CASES} + ${STEP8_BASELINE_CASES}")
workflow_slice_between(FRESH_CTEST STEP8_CMAKE "NAME ista_csr_real_differential_fresh_process" "set_tests_properties(")
string(REGEX MATCH [=[--repeats
    ([0-9]+)]=] FRESH_REPEAT "${FRESH_CTEST}")
if(FRESH_REPEAT STREQUAL "")
  message(FATAL_ERROR "Step 8 workflow fresh repeat argument differs")
endif()
set(STEP8_ACTUAL_REPEATS "${CMAKE_MATCH_1}")
math(EXPR STEP8_ACTUAL_PAIRS "${STEP8_ACTUAL_CASES} * ${STEP8_ACTUAL_REPEATS}")
math(EXPR STEP8_ACTUAL_LANES "2 * ${STEP8_ACTUAL_PAIRS}")
math(EXPR STEP8_ACTUAL_DIRFDS "1 + ${STEP8_ACTUAL_PAIRS} + ${STEP8_ACTUAL_LANES}")
string(REPLACE "|" ";" STEP8_FSYNC_COEFFICIENTS "${STEP8_ACTUAL_FSYNC_PACKED}")
list(GET STEP8_FSYNC_COEFFICIENTS 0 LANE_FSYNC_COEFFICIENT)
list(GET STEP8_FSYNC_COEFFICIENTS 1 SLOT_FSYNC_COEFFICIENT)
list(GET STEP8_FSYNC_COEFFICIENTS 2 ROOT_FSYNC_COEFFICIENT)
math(EXPR STEP8_ACTUAL_FSYNCS "${LANE_FSYNC_COEFFICIENT} * ${STEP8_ACTUAL_LANES} + ${SLOT_FSYNC_COEFFICIENT} * ${STEP8_ACTUAL_PAIRS} + ${ROOT_FSYNC_COEFFICIENT}")
workflow_read(REFERENCE_PROBE
              "${STEP8_DIR}/StaCsrRealReferenceProbe.cpp")
foreach(TOKEN
        "takeRawCheckpoint(database, \"pre_poison\""
        "test::poisonReferenceDerivedState("
        "takeRawCheckpoint(database, \"post_poison\""
        "buildCsrSessionSnapshot("
        "encodeSnapshotCandidate("
        "computeCsrSessionReference("
        "scatterCsrSessionTimingResult("
        "takeRawCheckpoint(database, \"post_scatter\""
        "installGuaranteedOverwriteCanaries("
        "takeRawCheckpoint(database, \"post_canary\""
        "runNativeExistingGraphTiming("
        "verifyGuaranteedOverwriteCanaries("
        "takeRawCheckpoint(database, \"post_rerun\""
        "releaseCsrSessionSnapshot(")
  workflow_require_count(REFERENCE_PROBE "${TOKEN}" 1
                         "reference phase token ${TOKEN}")
endforeach()
workflow_require_count(
    REFERENCE_PROBE "readNativeTiming(database, snapshot->input)" 3
    "reference native readback phase")
workflow_require_sequence(
    REFERENCE_PROBE "reference lane"
    "takeRawCheckpoint(database, \"pre_poison\""
    "test::poisonReferenceDerivedState("
    "takeRawCheckpoint(database, \"post_poison\""
    "buildCsrSessionSnapshot(" "encodeSnapshotCandidate("
    "computeCsrSessionReference(" "scatterCsrSessionTimingResult("
    "readNativeTiming(database, snapshot->input)"
    "takeRawCheckpoint(database, \"post_scatter\""
    "installGuaranteedOverwriteCanaries("
    "readNativeTiming(database, snapshot->input)"
    "takeRawCheckpoint(database, \"post_canary\""
    "runNativeExistingGraphTiming(" "verifyGuaranteedOverwriteCanaries("
    "readNativeTiming(database, snapshot->input)"
    "takeRawCheckpoint(database, \"post_rerun\""
    "releaseCsrSessionSnapshot(")
workflow_extract_labeled_sequence(
    STEP8_REFERENCE_PHASES REFERENCE_PROBE "reference phases"
    "pre_poison|RawCheckpoint pre_poison = takeRawCheckpoint"
    "poison|const std::vector<test::ReferencePoisonCanary> poison = test::poisonReferenceDerivedState"
    "build|CsrSessionSnapshotBuildResult build = buildCsrSessionSnapshot"
    "candidate|const EncodedSourceWire candidate = encodeSnapshotCandidate"
    "compute|CsrComputationResult computation = computeCsrSessionReference"
    "scatter|CsrTimingResultScatterAttempt scatter = scatterCsrSessionTimingResult"
    "readback|const NeutralWire scattered = readNativeTiming"
    "canary|Json canary_receipt = installGuaranteedOverwriteCanaries"
    "rerun|runNativeExistingGraphTiming(database)"
    "release|releaseCsrSessionSnapshot(*snapshot)")
workflow_extract_labeled_sequence(
    STEP8_ACTUAL_CHECKPOINTS REFERENCE_PROBE "reference checkpoints"
    "pre_poison|RawCheckpoint pre_poison = takeRawCheckpoint"
    "post_poison|RawCheckpoint post_poison = takeRawCheckpoint"
    "post_scatter|RawCheckpoint post_scatter = takeRawCheckpoint"
    "post_canary|RawCheckpoint post_canary = takeRawCheckpoint"
    "post_rerun|RawCheckpoint post_rerun = takeRawCheckpoint")
workflow_extract_labeled_sequence(
    STEP8_ACTUAL_CHECKPOINT_BINDINGS REFERENCE_PROBE "checkpoint publication"
    [=[pre_poison_json|{"pre_poison", std::move(pre_poison.json)}]=]
    [=[post_poison_json|{"post_poison", std::move(post_poison.json)}]=]
    [=[post_scatter_json|{"post_scatter", std::move(post_scatter.json)}]=]
    [=[post_canary_json|{"post_canary", std::move(post_canary.json)}]=]
    [=[post_rerun_json|{"post_rerun", std::move(post_rerun.json)}]=])
workflow_extract_labeled_sequence(
    STEP8_ACTUAL_PIPELINE_BINDINGS REFERENCE_PROBE "snapshot pipeline"
    "build_result|CsrSessionSnapshotBuildResult build = buildCsrSessionSnapshot(database, probeAnalysisContext(), session_token)"
    "built_variant|auto* built = std::get_if<std::unique_ptr<CsrCommittedSessionSnapshot>>(&build)"
    "committed_snapshot|std::unique_ptr<CsrCommittedSessionSnapshot> snapshot = std::move(*built)"
    "candidate_snapshot_input|const EncodedSourceWire candidate = encodeSnapshotCandidate(snapshot->input)"
    "compute_snapshot|CsrComputationResult computation = computeCsrSessionReference(*snapshot)"
    "result_outcome|auto* result = std::get_if<CsrTimingResult>(&computation.outcome)"
    "reference_snapshot_result|const NeutralWire reference = encodeReferenceTiming(snapshot->input, *result)"
    "scatter_snapshot_result|CsrTimingResultScatterAttempt scatter = scatterCsrSessionTimingResult(database, *snapshot, *result, probeAnalysisContext(), session_token)"
    [=[source_raw_bytes|{"pre_poison_raw_wire_hex", bytesToHex(pre_poison.bytes)}]=]
    [=[source_raw_sha|{"pre_poison_raw_sha256", pre_poison.sha256}]=]
    [=[source_candidate_wire|{"snapshot_candidate_wire_hex", candidate.wire_hex}]=]
    [=[source_candidate_sha|{"snapshot_candidate_sha256", candidate.sha256}]=])
workflow_read(SCENARIO "${STEP8_DIR}/CsrRealDifferentialScenario.cpp")
workflow_require_sequence(
    SCENARIO "shared scenario"
    "loadProbeDesign(options)" "session.initialize("
    "disableBeforeFull(database" "session.runFullBaseline()"
    "transformAfterFull(database" "runNativeExistingGraphTiming(database)"
    "execution.payload = work(" "session.close()")
workflow_require_count(SCENARIO "GraphBuilder" 0
                       "test-only direct GraphBuilder call")
foreach(TOKEN "DelayCalculator::getInst().init()"
              "ClockPropagator::getInst().propagate()"
              "TimingPropagator::getInst().propagate()"
              "TimingAnalyzer::getInst().analyze()")
  workflow_require_present(SCENARIO "${TOKEN}" "native rerun stage")
endforeach()
workflow_extract_labeled_sequence(
    STEP8_SCENARIO_PHASES SCENARIO "scenario phases"
    "initialize|throwOnStaFailure(session.initialize("
    "pre_full|pre = disableBeforeFull("
    "full|session.runFullBaseline()"
    "post_full|Json post = transformAfterFull("
    "native_initial|runNativeExistingGraphTiming(database)")
set(STEP8_ACTUAL_PHASES
    ${STEP8_SCENARIO_PHASES} ${STEP8_REFERENCE_PHASES})
file(GLOB_RECURSE PROHIBITED_ARTIFACTS LIST_DIRECTORIES FALSE
     "${STEP8_DIR}/__pycache__" "${STEP8_DIR}/*.pyc"
     "${STEP8_DIR}/*.tmp" "${STEP8_DIR}/oracle-result.json"
     "${STEP8_DIR}/reference-result.json"
     "${STEP8_DIR}/real-differential-controller-result.json")
if(EXISTS "${STEP8_DIR}/__pycache__")
  message(FATAL_ERROR "Step 8 source artifacts are prohibited: ${STEP8_DIR}/__pycache__")
endif()
if(PROHIBITED_ARTIFACTS)
  message(FATAL_ERROR "Step 8 source artifacts are prohibited: ${PROHIBITED_ARTIFACTS}")
endif()
workflow_check_detector(
    STEP8_ACTUAL_PHASES STEP8_ACTUAL_CHECKPOINTS STEP8_ACTUAL_POISON
    STEP8_ACTUAL_PUBLICATION STEP8_ACTUAL_CHECKPOINT_BINDINGS
    STEP8_ACTUAL_PIPELINE_BINDINGS "${STEP8_ACTUAL_REPEATS}" "${STEP8_ACTUAL_CASES}"
    "${STEP8_ACTUAL_PAIRS}" "${STEP8_ACTUAL_LANES}" "${STEP8_ACTUAL_DIRFDS}"
    "${STEP8_ACTUAL_FSYNCS}" "${STEP8_ACTUAL_COMMIT_PACKED}" STEP8_ACTUAL_MATRIX_ROWS STEP8_ACTUAL_SUMMARY_FLAGS STEP8_ACTUAL_CODEC STEP8_ACTUAL_KATS)
message(STATUS "CSR Step 8 workflow, process, fixture, and hygiene contract passed")
