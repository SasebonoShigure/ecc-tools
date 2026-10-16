#pragma once
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include "CsrCudaBackend.hpp"

namespace ista::csr::cuda_detail {
class CudaError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class CudaAllocationError final : public CudaError { public: using CudaError::CudaError; };

inline void check(cudaError_t error, const char* operation)
{
  if (error != cudaSuccess)
    throw CudaError(std::string(operation) + ": " + cudaGetErrorString(error));
}

inline void describeDevice(CsrCudaTelemetry& telemetry)
{
  int device = 0;
  check(cudaGetDevice(&device), "cudaGetDevice");
  cudaDeviceProp properties{};
  check(cudaGetDeviceProperties(&properties, device), "cudaGetDeviceProperties");
  telemetry.device_name = properties.name;
  telemetry.device_uuid = "GPU-";
  constexpr char hex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < sizeof(properties.uuid.bytes); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10)
      telemetry.device_uuid += '-';
    const auto byte = static_cast<unsigned char>(properties.uuid.bytes[index]);
    telemetry.device_uuid += hex[byte >> 4];
    telemetry.device_uuid += hex[byte & 15];
  }
  check(cudaDriverGetVersion(&telemetry.driver_version), "cudaDriverGetVersion");
  check(cudaRuntimeGetVersion(&telemetry.runtime_version), "cudaRuntimeGetVersion");
}

template <class T> struct DeviceBuffer
{
  T* data{};
  std::size_t capacity{};
};

// One session owns the stream, events and explicitly named device buffers.
class Runtime final
{
 public:
  explicit Runtime(CsrCudaTelemetry& telemetry) : _telemetry(&telemetry)
  {
    check(cudaStreamCreateWithFlags(&_stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags");
    try {
      for (auto& event : _events)
        check(cudaEventCreate(&event), "cudaEventCreate");
    } catch (...) {
      static_cast<void>(close());
      throw;
    }
  }
  ~Runtime() { static_cast<void>(close()); }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  cudaStream_t stream() const { return _stream; }
  void bindTelemetry(CsrCudaTelemetry& telemetry) noexcept { _telemetry = &telemetry; }

  template <class T> bool ensure(DeviceBuffer<T>& buffer, std::size_t count, std::size_t preserve = 0)
  {
    if (preserve > buffer.capacity)
      throw std::length_error("CUDA preserved range exceeds allocated capacity");
    if (count <= buffer.capacity)
      return false;
    const auto limit = std::numeric_limits<std::size_t>::max() / sizeof(T);
    if (count > limit)
      throw std::length_error("CUDA buffer byte capacity overflow");
    const auto extra = buffer.capacity / 2 + 1;
    const auto grown = extra <= limit - buffer.capacity ? buffer.capacity + extra : limit;
    if (buffer.data && _retired.size() == _retired.capacity())
      _retired.reserve(_allocations.size());
    auto* replacement = allocate<T>(std::max(count, grown));
    copy(buffer.data, replacement, preserve);
    // Copies and previous users stay ordered on the session stream. Retire all
    // grown buffers at the next required decision/delivery wait, not per array.
    if (buffer.data)
      _retired.push_back(buffer.data);
    buffer = {replacement, std::max(count, grown)};
    return true;
  }

  template <class T> T* allocate(std::size_t count)
  {
    if (count == 0)
      return nullptr;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::length_error("CUDA buffer byte capacity overflow");
    // Reserve bookkeeping before allocation so an allocation cannot leak if
    // growing the host ownership vector fails.
    auto slot = std::find(_allocations.begin(), _allocations.end(), nullptr);
    if (slot == _allocations.end()) {
      _allocations.push_back(nullptr);
      slot = _allocations.end() - 1;
    }
    const auto status = cudaMalloc(&*slot, count * sizeof(T));
    if (status == cudaErrorMemoryAllocation) {
      static_cast<void>(cudaGetLastError());
      throw CudaAllocationError("cudaMalloc: device capacity exhausted");
    }
    check(status, "cudaMalloc");
    ++_telemetry->allocations;
    return static_cast<T*>(*slot);
  }

  template <class T> void upload(const T* source, T* target, std::size_t count)
  {
    if (count == 0)
      return;
    check(cudaMemcpyAsync(target, source, count * sizeof(T), cudaMemcpyHostToDevice, _stream), "cudaMemcpyAsync H2D");
    _telemetry->h2d_bytes += count * sizeof(T);
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
    _telemetry->d2h_bytes += count * sizeof(T);
  }

  template <class T> void download(const T* source, std::vector<T>& target)
  {
    download(source, target.data(), target.size());
  }

  template <class T> void copy(const T* source, T* target, std::size_t count)
  {
    if (!count) return;
    check(cudaMemcpyAsync(target, source, count * sizeof(T), cudaMemcpyDeviceToDevice, _stream), "cudaMemcpyAsync D2D");
    _telemetry->d2d_bytes += count * sizeof(T);
  }
  template <class T> void zero(T* target, std::size_t count)
  {
    if (count) check(cudaMemsetAsync(target, 0, count * sizeof(T), _stream), "cudaMemsetAsync");
  }

  void record(std::size_t index) { check(cudaEventRecord(_events.at(index), _stream), "cudaEventRecord"); }
  void synchronize()
  {
    const auto started = std::chrono::steady_clock::now();
    const auto status = cudaStreamSynchronize(_stream);
    _telemetry->stream_wait_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    ++_telemetry->stream_waits;
    check(status, "cudaStreamSynchronize");
    for (void*& allocation : _retired) {
      if (!allocation)
        continue;
      const auto found = std::find(_allocations.begin(), _allocations.end(), allocation);
      if (found == _allocations.end())
        throw std::logic_error("CUDA retired buffer does not belong to this runtime");
      check(cudaFree(allocation), "cudaFree grown buffer");
      *found = nullptr;
      allocation = nullptr;
    }
    _retired.clear();
  }
  double milliseconds(std::size_t begin, std::size_t end) const
  {
    float value = 0.0F;
    check(cudaEventElapsedTime(&value, _events.at(begin), _events.at(end)), "cudaEventElapsedTime");
    return value;
  }

  // Explicit close reports asynchronous and cleanup failures; destruction is
  // only a no-throw fallback. Always attempt every resource after the first error.
  [[nodiscard]] cudaError_t close() noexcept
  {
    const auto retain = [&](cudaError_t status) {
      if (_close_status == cudaSuccess && status != cudaSuccess)
        _close_status = status;
    };
    if (_stream != nullptr)
      retain(cudaStreamSynchronize(_stream));
    for (auto& allocation : _allocations)
      if (allocation != nullptr)
        retain(cudaFree(std::exchange(allocation, nullptr)));
    for (auto& event : _events)
      if (event != nullptr)
        retain(cudaEventDestroy(std::exchange(event, nullptr)));
    if (_stream != nullptr)
      retain(cudaStreamDestroy(std::exchange(_stream, nullptr)));
    return _close_status;
  }

 private:
  cudaError_t _close_status{cudaSuccess};
  CsrCudaTelemetry* _telemetry;
  cudaStream_t _stream{nullptr};
  std::array<cudaEvent_t, 8> _events{};
  std::vector<void*> _allocations, _retired;
};


}  // namespace ista::csr::cuda_detail
