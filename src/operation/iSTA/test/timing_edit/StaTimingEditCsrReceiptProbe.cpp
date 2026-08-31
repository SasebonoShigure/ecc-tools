// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "StaTimingEditSessionProbeAccess.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

#include "CsrFullExactReceipt.hpp"
#include "../CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "NativeCandidateExecutor.hpp"

namespace ista::csr::test {

[[nodiscard]] CsrTimingResult materializeTimingResultFromNativeOracle(
    const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle);

}  // namespace ista::csr::test

namespace ista {

timing_edit::CsrFullExactReceipt StaTimingEditSessionProbeAccess::proveCsrFullExact(
    const timing_edit::NativeExecutionAuthority& authority,
    csr::CsrCommittedSessionSnapshot& snapshot, const Database& database,
    timing_edit::TransactionGeneration target_generation)
{
  if (!authority.active() || authority.sessionToken().value() == 0
      || target_generation.value() == 0
      || snapshot.input.source_identity.session_token
             != authority.sessionToken().value()) {
    throw std::invalid_argument(
        "CSR full exact proof requires an active matching authority and generation");
  }

  csr::CsrComputationResult computation
      = csr::computeCsrSessionReference(snapshot);
  auto* reference = std::get_if<csr::CsrTimingResult>(&computation.outcome);
  if (reference == nullptr
      || reference->source_identity != snapshot.input.source_identity) {
    throw std::runtime_error(
        "CSR CPU/full did not produce a result for the exact host input");
  }

  const csr::CsrNativeTimingOracle native_oracle
      = csr::test::readNativeTimingOracle(database);
  const csr::CsrTimingResult native
      = csr::test::materializeTimingResultFromNativeOracle(
          snapshot.input, native_oracle);
  if (native != *reference) {
    throw CsrFullExactMismatch{};
  }

  return timing_edit::CsrFullExactReceipt(
      authority.sessionToken(), target_generation,
      csr::canonicalReferenceInputBytes(snapshot.input), std::move(*reference));
}

}  // namespace ista
