#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>

namespace ista::test {
constexpr auto kWaitTimeout = std::chrono::seconds(5);
[[noreturn]] inline void failTimeout(const std::string& operation)
{
  std::cerr << operation << " timed out\n";
  std::quick_exit(2);
}
template <typename T>
T await(std::future<T>& result, const std::string& operation)
{
  if (result.wait_for(kWaitTimeout) != std::future_status::ready)
    failTimeout(operation);
  return result.get();
}
class BoundedGate final
{
 public:
  void signal()
  {
    {
      std::scoped_lock lock(_mutex);
      _signaled = true;
    }
    _condition.notify_all();
  }
  void wait(const std::string& operation)
  {
    std::unique_lock lock(_mutex);
    if (!_condition.wait_for(lock, kWaitTimeout, [this] { return _signaled; }))
      failTimeout(operation);
  }

 private:
  std::mutex _mutex;
  std::condition_variable _condition;
  bool _signaled{false};
};
}  // namespace ista::test
