add_library(ista_cuda STATIC
    CsrCudaPacking.cpp
    CsrCudaUnpacking.cpp
    CsrCudaRuntime.cu
    CsrCudaPropagation.cu
    CudaDualCsrSession.cu
    CudaDualCsrTransfer.cu
    CudaDualCsrPatch.cu
    CudaDualCsrKernels.cu
    CudaMixedLevelization.cu)
target_include_directories(ista_cuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ista_cuda PUBLIC ista_timing_csr_core ista_timing_dual_csr ista_timing_incremental_levelization PRIVATE CUDA::cudart)
target_compile_features(ista_cuda PUBLIC cxx_std_20)
set_target_properties(ista_cuda PROPERTIES
    CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON
    CUDA_RUNTIME_LIBRARY Shared POSITION_INDEPENDENT_CODE ON)
target_compile_options(ista_cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-pedantic;-ffp-contract=off>
    $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false;-Xcompiler=-Wall,-Wextra,-Werror>)
