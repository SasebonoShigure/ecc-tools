#pragma once
#include <cuda_runtime.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "CsrCudaBackend.hpp"

namespace ista::csr::cuda_detail {
class CudaError : public std::runtime_error { public: using std::runtime_error::runtime_error; };

inline void check(cudaError_t error, const char* operation)
{
  if (error != cudaSuccess)
    throw CudaError(std::string(operation) + ": " + cudaGetErrorString(error));
}

// One concrete owner holds the stream, events, and allocations. A full call
// or a detached targeted candidate releases its own storage on destruction.
class Runtime final
{
 public:
  explicit Runtime(CsrCudaTelemetry& telemetry) : _telemetry(telemetry)
  {
    check(cudaStreamCreateWithFlags(&_stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags");
    try {
      for (auto& event : _events)
        check(cudaEventCreate(&event), "cudaEventCreate");
    } catch (...) {
      release();
      throw;
    }
  }
  ~Runtime() { release(); }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  cudaStream_t stream() const { return _stream; }

  template <class T> T* allocate(std::size_t count)
  {
    if (count == 0)
      return nullptr;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::length_error("CUDA buffer byte capacity overflow");
    // Reserve bookkeeping before allocation so an allocation cannot leak if
    // growing the host ownership vector fails.
    _allocations.push_back(nullptr);
    check(cudaMalloc(&_allocations.back(), count * sizeof(T)), "cudaMalloc");
    ++_telemetry.allocations;
    return static_cast<T*>(_allocations.back());
  }

  template <class T> void upload(const T* source, T* target, std::size_t count)
  {
    if (count == 0)
      return;
    check(cudaMemcpyAsync(target, source, count * sizeof(T), cudaMemcpyHostToDevice, _stream), "cudaMemcpyAsync H2D");
    _telemetry.h2d_bytes += count * sizeof(T);
  }

  template <class T> void upload(const std::vector<T>& source, const T* target)
  {
    upload(source.data(), const_cast<T*>(target), source.size());
  }

  template <class T> void download(const T* source, T* target, std::size_t count)
  {
    if (count == 0)
      return;
    check(cudaMemcpyAsync(target, source, count * sizeof(T), cudaMemcpyDeviceToHost, _stream), "cudaMemcpyAsync D2H");
    _telemetry.d2h_bytes += count * sizeof(T);
  }

  template <class T> void download(const T* source, std::vector<T>& target)
  {
    download(source, target.data(), target.size());
  }

  template <class T> void copy(const T* source, T* target, std::size_t count)
  {
    if (!count) return;
    check(cudaMemcpyAsync(target, source, count * sizeof(T), cudaMemcpyDeviceToDevice, _stream), "cudaMemcpyAsync D2D");
    _telemetry.d2d_bytes += count * sizeof(T);
  }
  template <class T> void zero(T* target, std::size_t count)
  {
    if (count) check(cudaMemsetAsync(target, 0, count * sizeof(T), _stream), "cudaMemsetAsync");
  }

  void record(std::size_t index) { check(cudaEventRecord(_events.at(index), _stream), "cudaEventRecord"); }
  void synchronize() { check(cudaStreamSynchronize(_stream), "cudaStreamSynchronize"); }
  double milliseconds(std::size_t begin, std::size_t end) const
  {
    float value = 0.0F;
    check(cudaEventElapsedTime(&value, _events.at(begin), _events.at(end)), "cudaEventElapsedTime");
    return value;
  }

 private:
  void release() noexcept
  {
    if (_stream != nullptr)
      cudaStreamSynchronize(_stream);
    for (auto allocation : _allocations)
      if (allocation != nullptr)
        cudaFree(allocation);
    for (auto event : _events)
      if (event != nullptr)
        cudaEventDestroy(event);
    if (_stream != nullptr)
      cudaStreamDestroy(_stream);
  }

  CsrCudaTelemetry& _telemetry;
  cudaStream_t _stream{nullptr};
  std::array<cudaEvent_t, 7> _events{};
  std::vector<void*> _allocations;
};


}  // namespace ista::csr::cuda_detail
