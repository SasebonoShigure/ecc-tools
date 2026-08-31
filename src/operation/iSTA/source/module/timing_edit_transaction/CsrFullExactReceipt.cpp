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
#include "CsrFullExactReceipt.hpp"

#include <stdexcept>
#include <utility>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceTypes.hpp"

namespace ista::timing_edit {

struct CsrFullExactReceipt::Payload
{
  TimingEditSessionToken session_token;
  TransactionGeneration target_generation;
  std::vector<std::uint8_t> canonical_input;
  csr::CsrTimingResult exact_result;
};

CsrFullExactReceipt::CsrFullExactReceipt(
    TimingEditSessionToken session_token,
    TransactionGeneration target_generation,
    std::vector<std::uint8_t> canonical_input,
    csr::CsrTimingResult exact_result)
{
  if (session_token.value() == 0 || target_generation.value() == 0
      || canonical_input.empty()
      || exact_result.source_identity.session_token != session_token.value()) {
    throw std::invalid_argument(
        "CSR full exact receipt requires a matching nonzero session and input");
  }
  _payload = std::make_unique<Payload>(
      Payload{session_token, target_generation, std::move(canonical_input),
              std::move(exact_result)});
}

CsrFullExactReceipt::~CsrFullExactReceipt() noexcept = default;
CsrFullExactReceipt::CsrFullExactReceipt(CsrFullExactReceipt&&) noexcept = default;
CsrFullExactReceipt& CsrFullExactReceipt::operator=(
    CsrFullExactReceipt&&) noexcept = default;

bool CsrFullExactReceipt::matches(
    TimingEditSessionToken session_token,
    TransactionGeneration target_generation,
    const csr::CsrReferenceInput& input,
    const csr::CsrTimingResult& result) const
{
  if (_payload == nullptr || _payload->session_token != session_token
      || _payload->target_generation != target_generation
      || result.source_identity != input.source_identity
      || result.source_identity.session_token != session_token.value()) {
    return false;
  }
  return _payload->canonical_input == csr::canonicalReferenceInputBytes(input)
         && _payload->exact_result == result;
}

}  // namespace ista::timing_edit
