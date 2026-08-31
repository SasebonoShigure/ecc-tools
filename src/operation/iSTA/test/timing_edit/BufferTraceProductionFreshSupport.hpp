// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace ista::timing_edit {
struct CommittedEditIdentity;
class TimingEditCoordinator;
}

namespace ista::timing_edit::oracle {
struct ProductionFreshSessionView;
}

namespace ista::buffer_trace {

struct DerivedTimingFreshnessFunctionalReceipt
{
  bool exercised{false};
  std::string timing_point;
  double baseline_arrival{0.0};
  double tampered_arrival{0.0};
  double recovered_arrival{0.0};
  std::uint64_t csr_node_count{0};
  std::uint64_t csr_arc_count{0};
  bool tamper_observed{false};
  bool rerun_input_exact{false};
  bool rerun_cpu_exact{false};
  bool rerun_identity_exact{false};
  bool rollback_owner_restored{false};
  bool rollback_input_exact{false};
  bool rollback_cpu_exact{false};
  bool rollback_identity_exact{false};
  bool session_remained_baseline_executed{false};
  bool authority_remained_active{false};

  friend bool operator==(const DerivedTimingFreshnessFunctionalReceipt&,
                         const DerivedTimingFreshnessFunctionalReceipt&)
      = default;
};

namespace test_support {

[[nodiscard]] DerivedTimingFreshnessFunctionalReceipt
proveDerivedTimingFreshness(
    const timing_edit::oracle::ProductionFreshSessionView& view,
    const timing_edit::CommittedEditIdentity& initial_identity);

[[nodiscard]] bool proveFirstEmptyGenerationZero(
    const timing_edit::oracle::ProductionFreshSessionView& view,
    const timing_edit::TimingEditCoordinator& coordinator,
    const timing_edit::CommittedEditIdentity& initial_identity);

[[noreturn]] void rethrowTypedLoaderFailure(
    std::size_t callback_count, std::exception_ptr failure);

}  // namespace test_support
}  // namespace ista::buffer_trace
