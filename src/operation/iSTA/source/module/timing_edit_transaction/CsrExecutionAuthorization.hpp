// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "TimingEditTypes.hpp"

namespace ista {
class StaTimingEditSessionProbeAccess;
}

namespace ista::csr {
struct CsrReferenceInput;
struct CsrTimingResult;
}  // namespace ista::csr

namespace ista::timing_edit {

class TimingEditCoordinator;

// Move-only authorization minted by the guarded execution bridge. Full exact
// proof is present only in diagnostic builds and is never implied by authorization.
class CsrExecutionAuthorization final
{
 public:
  ~CsrExecutionAuthorization() noexcept;

  CsrExecutionAuthorization(const CsrExecutionAuthorization&) = delete;
  CsrExecutionAuthorization& operator=(const CsrExecutionAuthorization&) = delete;
  CsrExecutionAuthorization(CsrExecutionAuthorization&&) noexcept;
  CsrExecutionAuthorization& operator=(CsrExecutionAuthorization&&) noexcept;

  [[nodiscard]] bool hasFullExactProof() const noexcept { return _payload != nullptr; }

 private:
  struct Payload;
  CsrExecutionAuthorization(TimingEditSessionToken session_token, TransactionGeneration target_generation);
  [[nodiscard]] bool authorizes(TimingEditSessionToken session_token, TransactionGeneration generation) const noexcept;

  CsrExecutionAuthorization(TimingEditSessionToken session_token, TransactionGeneration target_generation, std::vector<std::uint8_t> canonical_input,
                            csr::CsrTimingResult exact_result);

  [[nodiscard]] bool matches(TimingEditSessionToken session_token, TransactionGeneration target_generation, const csr::CsrReferenceInput& input,
                             const csr::CsrTimingResult& result) const;

  TimingEditSessionToken _session_token;
  TransactionGeneration _target_generation;
  std::unique_ptr<Payload> _payload;

  friend class ::ista::StaTimingEditSessionProbeAccess;
  friend class TimingEditCoordinator;
};

}  // namespace ista::timing_edit
