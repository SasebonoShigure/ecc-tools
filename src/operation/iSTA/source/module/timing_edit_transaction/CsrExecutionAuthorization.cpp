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
#include "CsrExecutionAuthorization.hpp"

#include <stdexcept>
#include <utility>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceTypes.hpp"

namespace ista::timing_edit {

struct CsrExecutionAuthorization::Payload
{
  std::vector<std::uint8_t> canonical_input;
  csr::CsrTimingResult exact_result;
};

CsrExecutionAuthorization::CsrExecutionAuthorization(TimingEditSessionToken session_token, TransactionGeneration target_generation)
    : _session_token(session_token), _target_generation(target_generation)
{
  if (session_token.value() == 0 || target_generation.value() == 0)
    throw std::invalid_argument("execution authorization requires a nonzero binding");
}

bool CsrExecutionAuthorization::authorizes(TimingEditSessionToken session_token, TransactionGeneration generation) const noexcept
{
  return _session_token == session_token && _target_generation == generation;
}

CsrExecutionAuthorization::CsrExecutionAuthorization(TimingEditSessionToken session_token, TransactionGeneration target_generation,
                                                     std::vector<std::uint8_t> canonical_input, csr::CsrTimingResult exact_result)
    : CsrExecutionAuthorization(session_token, target_generation)
{
  if (canonical_input.empty() || exact_result.source_identity.session_token != session_token.value()) {
    throw std::invalid_argument("CSR full exact receipt requires a matching nonzero session and input");
  }
  _payload = std::make_unique<Payload>(Payload{std::move(canonical_input), std::move(exact_result)});
}

CsrExecutionAuthorization::~CsrExecutionAuthorization() noexcept = default;
CsrExecutionAuthorization::CsrExecutionAuthorization(CsrExecutionAuthorization&& other) noexcept
    : _session_token(std::exchange(other._session_token, {})),
      _target_generation(std::exchange(other._target_generation, {})),
      _payload(std::move(other._payload))
{
}

CsrExecutionAuthorization& CsrExecutionAuthorization::operator=(CsrExecutionAuthorization&& other) noexcept
{
  if (this != &other) {
    _session_token = std::exchange(other._session_token, {});
    _target_generation = std::exchange(other._target_generation, {});
    _payload = std::move(other._payload);
  }
  return *this;
}

bool CsrExecutionAuthorization::matches(TimingEditSessionToken session_token, TransactionGeneration target_generation, const csr::CsrReferenceInput& input,
                                        const csr::CsrTimingResult& result) const
{
  if (_payload == nullptr || !authorizes(session_token, target_generation)
      || result.source_identity != input.source_identity || result.source_identity.session_token != session_token.value()) {
    return false;
  }
  return _payload->canonical_input == csr::canonicalReferenceInputBytes(input) && _payload->exact_result == result;
}

}  // namespace ista::timing_edit
