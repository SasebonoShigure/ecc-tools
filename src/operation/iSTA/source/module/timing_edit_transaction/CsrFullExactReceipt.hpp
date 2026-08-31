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

// A successful Stage-3 CPU/full proof. The payload is intentionally opaque and
// move-only: only the test-only guarded session bridge can mint one after an
// independent native timing readback agrees with the complete CSR result.
class CsrFullExactReceipt final
{
 public:
  ~CsrFullExactReceipt() noexcept;

  CsrFullExactReceipt(const CsrFullExactReceipt&) = delete;
  CsrFullExactReceipt& operator=(const CsrFullExactReceipt&) = delete;
  CsrFullExactReceipt(CsrFullExactReceipt&&) noexcept;
  CsrFullExactReceipt& operator=(CsrFullExactReceipt&&) noexcept;

 private:
  struct Payload;

  CsrFullExactReceipt(TimingEditSessionToken session_token,
                      TransactionGeneration target_generation,
                      std::vector<std::uint8_t> canonical_input,
                      csr::CsrTimingResult exact_result);

  [[nodiscard]] bool matches(TimingEditSessionToken session_token,
                             TransactionGeneration target_generation,
                             const csr::CsrReferenceInput& input,
                             const csr::CsrTimingResult& result) const;

  std::unique_ptr<Payload> _payload;

  friend class ::ista::StaTimingEditSessionProbeAccess;
  friend class TimingEditCoordinator;
};

}  // namespace ista::timing_edit
