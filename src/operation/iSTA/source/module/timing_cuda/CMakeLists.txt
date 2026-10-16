add_library(ista_cuda STATIC
    CsrCudaPacking.cpp
    CsrCudaParasiticPacking.cpp
    CsrCudaUnpacking.cpp
    CsrCudaRuntime.cu
    CsrCudaLevelization.cu
    CsrCudaPropagation.cu
    CudaDualCsrSession.cu
    CudaBufferCandidateTiming.cu
    CudaDualCsrTransfer.cu
    CudaDualCsrParasitics.cu
    CudaDualCsrRcPatch.cu
    CudaDualCsrRcTransfer.cu
    CudaDualCsrPatch.cu
    CudaDualCsrKernels.cu
    CudaMixedLevelization.cu)
# Split Toolkit installations may provide CCCL headers separately from cudart.
find_path(ISTA_CUDA_CCCL_INCLUDE_DIR cub/device/device_scan.cuh
    HINTS ${CUDAToolkit_INCLUDE_DIRS} REQUIRED)
target_include_directories(ista_cuda SYSTEM PRIVATE ${ISTA_CUDA_CCCL_INCLUDE_DIR})
target_include_directories(ista_cuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ista_cuda PUBLIC ista_timing_csr_core ista_timing_dual_csr ista_timing_incremental_levelization PRIVATE CUDA::cudart)
target_compile_features(ista_cuda PUBLIC cxx_std_20)
set_target_properties(ista_cuda PROPERTIES
    CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON
    CUDA_RUNTIME_LIBRARY Shared POSITION_INDEPENDENT_CODE ON)
target_compile_options(ista_cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-pedantic;-ffp-contract=off>
    $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false;-Xcompiler=-Wall,-Wextra,-Werror>)
