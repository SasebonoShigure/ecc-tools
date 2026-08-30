# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

  set(ISTA_CSR_CORE_EXPECTED_SOURCES
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingSemanticKey.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrFingerprint.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceCanonicalEncoder.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceFingerprintPayload.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrSnapshotIdentityValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrFifoLevelization.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceInputValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceConstraintValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceNativeModelValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTimingResultValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTimingResultArcValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTableLookup.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceBackend.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceResultInitialization.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceArcTransfer.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceForwardPropagation.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceRequiredPropagation.cpp"
  )
  get_target_property(ISTA_CSR_CORE_ACTUAL_SOURCES ista_timing_csr_core SOURCES)
  ista_assert_same_inventory(
      ISTA_CSR_CORE_ACTUAL_SOURCES ISTA_CSR_CORE_EXPECTED_SOURCES
      "ista_timing_csr_core source inventory"
  )

  set(ISTA_CSR_CORE_FILES
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferencePrimitives.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceNativeModels.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceTypes.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingSemanticKey.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingSemanticKey.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrFingerprint.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrFingerprint.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceCanonicalEncoder.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceCanonicalEncoderInternal.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceCanonicalEncoder.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceFingerprintPayload.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceFingerprintPayload.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidator.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidatorDetail.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceInputValidatorInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrFifoLevelization.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrFifoLevelization.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrSnapshotIdentityValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceInputValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceConstraintValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceNativeModelValidatorInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceNativeModelValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTimingResultValidatorInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTimingResultValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTimingResultArcValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTableLookup.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTableLookup.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceBackend.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceBackendInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceBackend.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceResultInitialization.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceArcTransfer.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceForwardPropagation.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceRequiredPropagation.cpp"
  )
  set(ISTA_CSR_CORE_OBJECTS
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrReferenceValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrSnapshotIdentityValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrFifoLevelization.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrReferenceInputValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrReferenceConstraintValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrReferenceNativeModelValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrTimingResultValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrTimingResultArcValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrTableLookup.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrCpuReferenceBackend.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrCpuReferenceResultInitialization.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrCpuReferenceArcTransfer.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrCpuReferenceForwardPropagation.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/CsrCpuReferenceRequiredPropagation.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/__/__/data_manager/adapter/csr/CsrFingerprint.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/__/__/data_manager/adapter/csr/CsrReferenceCanonicalEncoder.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/__/__/data_manager/adapter/csr/CsrReferenceFingerprintPayload.cpp.o"
      "src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir/__/__/data_manager/adapter/csr/TimingSemanticKey.cpp.o"
  )
  string(JOIN "|" ISTA_CSR_CORE_ACTUAL_SOURCES_PACKED ${ISTA_CSR_CORE_ACTUAL_SOURCES})
  string(JOIN "|" ISTA_CSR_CORE_EXPECTED_SOURCES_PACKED ${ISTA_CSR_CORE_EXPECTED_SOURCES})
  string(JOIN "|" ISTA_CSR_CORE_FILES_PACKED ${ISTA_CSR_CORE_FILES})
  string(JOIN "|" ISTA_CSR_CORE_OBJECTS_PACKED ${ISTA_CSR_CORE_OBJECTS})
  add_test(
      NAME ista_csr_core_include_isolation
      COMMAND
      "${CMAKE_COMMAND}"
      "-DCSR_CORE_ACTUAL_SOURCES_PACKED=${ISTA_CSR_CORE_ACTUAL_SOURCES_PACKED}"
      "-DCSR_CORE_EXPECTED_SOURCES_PACKED=${ISTA_CSR_CORE_EXPECTED_SOURCES_PACKED}"
      "-DCSR_CORE_FILES_PACKED=${ISTA_CSR_CORE_FILES_PACKED}"
      "-DCSR_CORE_OBJECTS_PACKED=${ISTA_CSR_CORE_OBJECTS_PACKED}"
      "-DCSR_CORE_OBJECT_DIR=src/operation/iSTA/source/module/timing_csr/CMakeFiles/ista_timing_csr_core.dir"
      "-DCSR_BUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DNINJA_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
      "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
      "-DCSR_GENERATOR=${CMAKE_GENERATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrCoreIsolation.cmake"
  )
  set_tests_properties(
      ista_csr_core_include_isolation
      PROPERTIES LABELS "ista_unit"
  )

  set(ISTA_CSR_SNAPSHOT_EXPECTED_SOURCES
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotStorage.cpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationTransition.cpp"
  )
  get_target_property(
      ISTA_CSR_SNAPSHOT_ACTUAL_SOURCES ista_timing_csr_snapshot_contract SOURCES
  )
  ista_assert_same_inventory(
      ISTA_CSR_SNAPSHOT_ACTUAL_SOURCES ISTA_CSR_SNAPSHOT_EXPECTED_SOURCES
      "ista_timing_csr_snapshot_contract source inventory"
  )
  set(ISTA_CSR_SNAPSHOT_EXPECTED_OBJECTS
      "src/operation/iSTA/source/module/timing_csr_snapshot/CMakeFiles/ista_timing_csr_snapshot_contract.dir/CsrSnapshotStorage.cpp.o"
      "src/operation/iSTA/source/module/timing_csr_snapshot/CMakeFiles/ista_timing_csr_snapshot_contract.dir/CsrSnapshotObservationValidator.cpp.o"
      "src/operation/iSTA/source/module/timing_csr_snapshot/CMakeFiles/ista_timing_csr_snapshot_contract.dir/CsrSnapshotObservationTransition.cpp.o"
  )
  set(ISTA_CSR_SNAPSHOT_ALLOWED_FILES
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrCaptureMetadata.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrFingerprint.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceCanonicalEncoder.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceNativeModels.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferencePrimitives.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceTypes.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingSemanticKey.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrFifoLevelization.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidator.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservation.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotInvalidationPolicyInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationTransition.cpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationTransition.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationValidator.cpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationValidator.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotStorage.cpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotStorage.hpp"
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_SNAPSHOT_LINKS ista_timing_csr_snapshot_contract LINK_LIBRARIES
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_SNAPSHOT_INTERFACE_LINKS
      ista_timing_csr_snapshot_contract INTERFACE_LINK_LIBRARIES
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_SNAPSHOT_INTERFACE_INCLUDES
      ista_timing_csr_snapshot_contract INTERFACE_INCLUDE_DIRECTORIES
  )
  set(ISTA_CSR_EXPECTED_SNAPSHOT_LINKS ista_timing_csr_core)
  set(
      ISTA_CSR_EXPECTED_SNAPSHOT_INCLUDES
      "${ISTA_SOURCE}/module/timing_csr_snapshot"
  )
  ista_assert_same_inventory(
      ISTA_CSR_SNAPSHOT_LINKS ISTA_CSR_EXPECTED_SNAPSHOT_LINKS
      "CSR snapshot direct links"
  )
  ista_assert_same_inventory(
      ISTA_CSR_SNAPSHOT_INTERFACE_LINKS ISTA_CSR_EXPECTED_SNAPSHOT_LINKS
      "CSR snapshot interface links"
  )
  ista_assert_same_inventory(
      ISTA_CSR_SNAPSHOT_INTERFACE_INCLUDES ISTA_CSR_EXPECTED_SNAPSHOT_INCLUDES
      "CSR snapshot interface include directories"
  )
  foreach(SNAPSHOT_FORBIDDEN_TARGET IN ITEMS ista_source ista_module ista_interface)
    foreach(SNAPSHOT_FORBIDDEN_PROPERTY IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      ista_get_target_property_or_empty(
          SNAPSHOT_FORBIDDEN_LINKS
          "${SNAPSHOT_FORBIDDEN_TARGET}" "${SNAPSHOT_FORBIDDEN_PROPERTY}"
      )
      string(
          FIND "${SNAPSHOT_FORBIDDEN_LINKS}"
          "ista_timing_csr_snapshot_contract" SNAPSHOT_LINK_POSITION
      )
      if(NOT SNAPSHOT_LINK_POSITION EQUAL -1)
        message(
            FATAL_ERROR
            "${SNAPSHOT_FORBIDDEN_TARGET}.${SNAPSHOT_FORBIDDEN_PROPERTY} must not aggregate the Step 3 snapshot contract"
        )
      endif()
    endforeach()
  endforeach()
  string(JOIN "|" ISTA_CSR_SNAPSHOT_ALLOWED_FILES_PACKED ${ISTA_CSR_SNAPSHOT_ALLOWED_FILES})
  string(JOIN "|" ISTA_CSR_SNAPSHOT_ACTUAL_SOURCES_PACKED ${ISTA_CSR_SNAPSHOT_ACTUAL_SOURCES})
  string(JOIN "|" ISTA_CSR_SNAPSHOT_EXPECTED_SOURCES_PACKED ${ISTA_CSR_SNAPSHOT_EXPECTED_SOURCES})
  string(JOIN "|" ISTA_CSR_SNAPSHOT_EXPECTED_OBJECTS_PACKED ${ISTA_CSR_SNAPSHOT_EXPECTED_OBJECTS})
  add_test(
      NAME ista_csr_snapshot_contract_include_isolation
      COMMAND
      "${CMAKE_COMMAND}"
      "-DCSR_SNAPSHOT_ALLOWED_FILES_PACKED=${ISTA_CSR_SNAPSHOT_ALLOWED_FILES_PACKED}"
      "-DCSR_SNAPSHOT_ACTUAL_SOURCES_PACKED=${ISTA_CSR_SNAPSHOT_ACTUAL_SOURCES_PACKED}"
      "-DCSR_SNAPSHOT_EXPECTED_SOURCES_PACKED=${ISTA_CSR_SNAPSHOT_EXPECTED_SOURCES_PACKED}"
      "-DCSR_SNAPSHOT_EXPECTED_OBJECTS_PACKED=${ISTA_CSR_SNAPSHOT_EXPECTED_OBJECTS_PACKED}"
      "-DCSR_SNAPSHOT_OBJECT_DIR=src/operation/iSTA/source/module/timing_csr_snapshot/CMakeFiles/ista_timing_csr_snapshot_contract.dir"
      "-DCSR_BUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DNINJA_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
      "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
      "-DCSR_GENERATOR=${CMAKE_GENERATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrSnapshotContractIsolation.cmake"
  )
  set_tests_properties(
      ista_csr_snapshot_contract_include_isolation PROPERTIES LABELS "ista_unit"
  )

  set(ISTA_CSR_ADAPTER_EXPECTED_SOURCES
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphEncoding.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphValidator.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrAdapter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrArcExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrAdjacency.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/NativeGraphProjection.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceInputAdapter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterValidation.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterPreparation.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterPublish.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrConstraintExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrClockSeedExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrTimingCheckExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrLibertyExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrLoadExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrParasiticExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrSessionSnapshotAdapter.cpp"
  )
  get_target_property(ISTA_CSR_ADAPTER_ACTUAL_SOURCES ista_timing_graph_csr_adapter SOURCES)
  ista_assert_same_inventory(
      ISTA_CSR_ADAPTER_ACTUAL_SOURCES ISTA_CSR_ADAPTER_EXPECTED_SOURCES
      "ista_timing_graph_csr_adapter source inventory"
  )
  set(ISTA_CSR_ADAPTER_EXPECTED_OBJECTS
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrGraphEncoding.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrGraphValidator.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingGraphCsrAdapter.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingGraphCsrArcExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingGraphCsrAdjacency.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/NativeGraphProjection.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrReferenceInputAdapter.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatter.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatterValidation.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatterPreparation.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/TimingResultScatterPublish.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrConstraintExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrClockSeedExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrTimingCheckExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrLibertyExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrLoadExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrParasiticExport.cpp.o"
      "src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir/CsrSessionSnapshotAdapter.cpp.o"
  )
  set(ISTA_CSR_ADAPTER_ALLOWED_FILES
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrCaptureMetadata.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrClockSeedExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrConstraintExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrFingerprint.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphEncoding.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphEncoding.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphTypes.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphValidator.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrGraphValidator.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrNativeGraphOracle.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceCanonicalEncoder.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceFingerprintPayload.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceInputAdapterInternal.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceNativeModels.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferencePrimitives.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceTypes.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrReferenceInputAdapter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrSessionSnapshotAdapter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrSessionSnapshotAdapter.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrSessionSnapshotAdapterInternal.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrTimingCheckExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrLibertyExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrLoadExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/CsrParasiticExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/NativeGraphProjection.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/NativeGraphProjection.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrAdapter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrAdapter.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrAdapterInternal.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrAdjacency.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingGraphCsrArcExport.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingSemanticKey.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatter.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatter.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterInternal.hpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterValidation.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterPreparation.cpp"
      "${ISTA_SOURCE}/data_manager/adapter/csr/TimingResultScatterPublish.cpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidatorDetail.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrTimingResultValidatorInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceNativeModelValidatorInternal.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrReferenceValidator.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrCpuReferenceBackend.hpp"
      "${ISTA_SOURCE}/module/timing_csr/CsrFifoLevelization.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservation.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationTransition.hpp"
      "${ISTA_SOURCE}/module/timing_csr_snapshot/CsrSnapshotObservationValidator.hpp"
      "${ISTA_SOURCE}/data_manager/advance/AnalysisType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/Arc.hpp"
      "${ISTA_SOURCE}/data_manager/advance/ArcType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/Database.hpp"
      "${ISTA_SOURCE}/data_manager/advance/Instance.hpp"
      "${ISTA_SOURCE}/data_manager/advance/InstancePower.hpp"
      "${ISTA_SOURCE}/data_manager/advance/LogicExpression.hpp"
      "${ISTA_SOURCE}/data_manager/advance/LogicExpressionTerm.hpp"
      "${ISTA_SOURCE}/data_manager/advance/LogicOperationType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/Net.hpp"
      "${ISTA_SOURCE}/data_manager/advance/ParasiticLibrary.hpp"
      "${ISTA_SOURCE}/data_manager/advance/ParasiticNet.hpp"
      "${ISTA_SOURCE}/data_manager/advance/ParasiticNode.hpp"
      "${ISTA_SOURCE}/data_manager/advance/ParasiticResistor.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PathSourceType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/Pin.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PinDirection.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PowerActivity.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PowerActivityOrigin.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PowerGroupType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PowerSummary.hpp"
      "${ISTA_SOURCE}/data_manager/advance/PowerValue.hpp"
      "${ISTA_SOURCE}/data_manager/advance/STAHeader.hpp"
      "${ISTA_SOURCE}/data_manager/advance/Summary.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingArc.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingArcSense.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingCapacitiveUnit.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingCell.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingCellArc.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingCellPort.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingCheckArc.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingCheckType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingClock.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingConstraint.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingLeakagePower.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingLibrary.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPath.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPathEnd.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPathGroup.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPathPoint.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPathState.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPoint.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPortConstraint.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingPowerArc.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingResistanceUnit.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingTable.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingTableVariableType.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TimingTimeUnit.hpp"
      "${ISTA_SOURCE}/data_manager/advance/TransType.hpp"
      "${ISTA_SOURCE}/toolkit/logger/LogLevel.hpp"
      "${ISTA_SOURCE}/toolkit/logger/Logger.hpp"
  )
  set(ISTA_CSR_DIRECT_PROJECTOR_FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/CsrNativeSemanticKeyTestSupport.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/CsrNativeGraphTestSupport.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/CsrNativeGraphRawProjector.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/CsrNativeGraphOracleReader.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/CsrNativeGraphIndependentEncoding.cpp"
  )

  ista_get_target_property_or_empty(ISTA_CSR_CORE_LINKS ista_timing_csr_core LINK_LIBRARIES)
  ista_get_target_property_or_empty(
      ISTA_CSR_CORE_INTERFACE_LINKS ista_timing_csr_core INTERFACE_LINK_LIBRARIES
  )
  ista_get_target_property_or_empty(ISTA_CSR_CORE_OPTIONS ista_timing_csr_core COMPILE_OPTIONS)
  ista_get_target_property_or_empty(ISTA_CSR_HEADER_LINKS ista_timing_database_headers LINK_LIBRARIES)
  ista_get_target_property_or_empty(
      ISTA_CSR_HEADER_INTERFACE_LINKS ista_timing_database_headers INTERFACE_LINK_LIBRARIES
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_ADAPTER_LINKS ista_timing_graph_csr_adapter LINK_LIBRARIES
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_ADAPTER_INTERFACE_LINKS ista_timing_graph_csr_adapter INTERFACE_LINK_LIBRARIES
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_HEADER_INTERFACE_INCLUDES ista_timing_database_headers INTERFACE_INCLUDE_DIRECTORIES
  )
  ista_get_target_property_or_empty(
      ISTA_CSR_ADAPTER_INTERFACE_INCLUDES ista_timing_graph_csr_adapter INTERFACE_INCLUDE_DIRECTORIES
  )
  set(ISTA_CSR_EXPECTED_EMPTY)
  set(ISTA_CSR_EXPECTED_CORE_OPTIONS -ffp-contract=off)
  set(
      ISTA_CSR_EXPECTED_ADAPTER_LINKS
      ista_timing_csr_core
      ista_timing_csr_snapshot_contract
      ista_timing_database_headers
  )
  set(
      ISTA_CSR_EXPECTED_ADAPTER_INTERFACE_LINKS
      "ista_timing_csr_core;ista_timing_csr_snapshot_contract;$<LINK_ONLY:ista_timing_database_headers>"
  )
  set(ISTA_CSR_EXPECTED_HEADER_INTERFACE_INCLUDES
      "${ISTA_SOURCE}/data_manager/advance"
      "${ISTA_SOURCE}/data_manager/basic"
      "${ISTA_SOURCE}/toolkit/logger"
  )
  set(ISTA_CSR_EXPECTED_ADAPTER_INTERFACE_INCLUDES "${ISTA_SOURCE}/data_manager/adapter/csr")
  ista_assert_same_inventory(ISTA_CSR_CORE_LINKS ISTA_CSR_EXPECTED_EMPTY "CSR core direct links")
  ista_assert_same_inventory(
      ISTA_CSR_CORE_INTERFACE_LINKS ISTA_CSR_EXPECTED_EMPTY "CSR core interface links"
  )
  ista_assert_same_inventory(
      ISTA_CSR_CORE_OPTIONS ISTA_CSR_EXPECTED_CORE_OPTIONS "CSR core compile options"
  )
  ista_assert_same_inventory(ISTA_CSR_HEADER_LINKS ISTA_CSR_EXPECTED_EMPTY "CSR header direct links")
  ista_assert_same_inventory(
      ISTA_CSR_HEADER_INTERFACE_LINKS ISTA_CSR_EXPECTED_EMPTY "CSR header interface links"
  )
  ista_assert_same_inventory(
      ISTA_CSR_ADAPTER_LINKS ISTA_CSR_EXPECTED_ADAPTER_LINKS "CSR adapter direct links"
  )
  ista_assert_same_inventory(
      ISTA_CSR_ADAPTER_INTERFACE_LINKS ISTA_CSR_EXPECTED_ADAPTER_INTERFACE_LINKS
      "CSR adapter interface links"
  )
  ista_assert_same_inventory(
      ISTA_CSR_HEADER_INTERFACE_INCLUDES ISTA_CSR_EXPECTED_HEADER_INTERFACE_INCLUDES
      "CSR header interface include directories"
  )
  ista_assert_same_inventory(
      ISTA_CSR_ADAPTER_INTERFACE_INCLUDES ISTA_CSR_EXPECTED_ADAPTER_INTERFACE_INCLUDES
      "CSR adapter interface include directories"
  )
  foreach(AGGREGATE_TARGET IN ITEMS ista_source ista_module)
    foreach(AGGREGATE_PROPERTY IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      ista_get_target_property_or_empty(
          AGGREGATE_LINKS "${AGGREGATE_TARGET}" "${AGGREGATE_PROPERTY}"
      )
      string(FIND "${AGGREGATE_LINKS}" "ista_timing_graph_csr_adapter" ADAPTER_POSITION)
      if(NOT ADAPTER_POSITION EQUAL -1)
        message(
            FATAL_ERROR
            "${AGGREGATE_TARGET}.${AGGREGATE_PROPERTY} must not aggregate the Step 2 adapter"
        )
      endif()
    endforeach()
  endforeach()

  string(JOIN "|" ISTA_CSR_ADAPTER_ALLOWED_FILES_PACKED ${ISTA_CSR_ADAPTER_ALLOWED_FILES})
  string(JOIN "|" ISTA_CSR_ADAPTER_ACTUAL_SOURCES_PACKED ${ISTA_CSR_ADAPTER_ACTUAL_SOURCES})
  string(JOIN "|" ISTA_CSR_ADAPTER_EXPECTED_SOURCES_PACKED ${ISTA_CSR_ADAPTER_EXPECTED_SOURCES})
  string(JOIN "|" ISTA_CSR_ADAPTER_EXPECTED_OBJECTS_PACKED ${ISTA_CSR_ADAPTER_EXPECTED_OBJECTS})
  string(JOIN "|" ISTA_CSR_DIRECT_PROJECTOR_FILES_PACKED ${ISTA_CSR_DIRECT_PROJECTOR_FILES})
  add_test(
      NAME ista_csr_adapter_include_isolation
      COMMAND
      "${CMAKE_COMMAND}"
      "-DCSR_ADAPTER_ALLOWED_FILES_PACKED=${ISTA_CSR_ADAPTER_ALLOWED_FILES_PACKED}"
      "-DCSR_ADAPTER_ACTUAL_SOURCES_PACKED=${ISTA_CSR_ADAPTER_ACTUAL_SOURCES_PACKED}"
      "-DCSR_ADAPTER_EXPECTED_SOURCES_PACKED=${ISTA_CSR_ADAPTER_EXPECTED_SOURCES_PACKED}"
      "-DCSR_ADAPTER_EXPECTED_OBJECTS_PACKED=${ISTA_CSR_ADAPTER_EXPECTED_OBJECTS_PACKED}"
      "-DCSR_ADAPTER_OBJECT_DIR=src/operation/iSTA/source/data_manager/adapter/csr/CMakeFiles/ista_timing_graph_csr_adapter.dir"
      "-DCSR_DIRECT_PROJECTOR_FILES_PACKED=${ISTA_CSR_DIRECT_PROJECTOR_FILES_PACKED}"
      "-DCSR_TEST_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DNM_EXECUTABLE=${CMAKE_NM}"
      "-DCSR_BUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DNINJA_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
      "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
      "-DCSR_GENERATOR=${CMAKE_GENERATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrAdapterIsolation.cmake"
  )
  set_tests_properties(
      ista_csr_adapter_include_isolation PROPERTIES LABELS "ista_unit"
  )

  add_test(
      NAME ista_csr_step3_source_inventory
      COMMAND
      "${CMAKE_COMMAND}"
      "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
      "-DCSR_TEST_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrStep3SourceInventory.cmake"
  )
  set_tests_properties(
      ista_csr_step3_source_inventory
      PROPERTIES LABELS "ista_unit"
  )
