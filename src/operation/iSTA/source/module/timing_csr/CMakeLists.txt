set(ISTA_TIMING_CSR_ADAPTER ${ISTA_DATA_MANAGER}/adapter/csr)

add_library(ista_timing_csr_core
    ${ISTA_TIMING_CSR_ADAPTER}/TimingSemanticKey.cpp
    ${ISTA_TIMING_CSR_ADAPTER}/CsrFingerprint.cpp
    ${ISTA_TIMING_CSR_ADAPTER}/CsrReferenceCanonicalEncoder.cpp
    ${ISTA_TIMING_CSR_ADAPTER}/CsrReferenceFingerprintPayload.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrSnapshotIdentityValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrFifoLevelization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceInputValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceConstraintValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceNativeModelValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrTimingResultValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrTimingResultArcValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrTableLookup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrCpuReferenceBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrCpuReferenceResultInitialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrCpuReferenceArcTransfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrCpuReferenceForwardPropagation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrCpuReferenceRequiredPropagation.cpp
)

target_include_directories(ista_timing_csr_core
    PUBLIC
    ${ISTA_TIMING_CSR_ADAPTER}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(ista_timing_csr_core PRIVATE -ffp-contract=off)

# This target is intentionally DB-free.  Do not add ista_data_manager,
# ista_module, ista_interface, native oracle, or capture metadata dependencies.
