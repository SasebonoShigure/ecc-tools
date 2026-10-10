#pragma once
#include <cstddef>

namespace ista_test_allocation {
void armCountOnly() noexcept;
void armFailure(std::size_t ordinal) noexcept;
void armFailureWhen(bool (*condition)(void*) noexcept, void* context) noexcept;
[[nodiscard]] bool failureInjected() noexcept;
[[nodiscard]] std::size_t disable() noexcept;
}  // namespace ista_test_allocation
