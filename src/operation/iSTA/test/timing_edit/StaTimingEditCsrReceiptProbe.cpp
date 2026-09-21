// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <stdexcept>
#include <utility>
#include <variant>

#include "../CsrNativeTimingOracleTestSupport.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"

namespace ista::csr::test {

[[nodiscard]] CsrTimingResult materializeTimingResultFromNativeOracle(
    const CsrReferenceInput& input, const CsrNativeTimingOracle& oracle);

}  // namespace ista::csr::test

namespace ista {

void StaTimingEditSessionProbeAccess::requireNativeTimingEqual(
    const csr::CsrReferenceInput& input, const csr::CsrTimingResult& reference,
    const Database& database)
{
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("CSR exact comparison requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
  if (reference.source_identity != input.source_identity)
    throw std::invalid_argument("CSR timing result belongs to a different input");
  const auto native_oracle = csr::test::readNativeTimingOracle(database);
  const auto native = csr::test::materializeTimingResultFromNativeOracle(input, native_oracle);
  if (native != reference)
    throw CsrFullExactMismatch{};
}

void StaTimingEditSessionProbeAccess::requireCsrFullExact(
    csr::CsrCommittedSessionSnapshot& snapshot, const Database& database)
{
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("CSR exact comparison requires ECC_INCREGPUSTA_DEBUG_CHECKS=ON");
  auto computation = csr::computeCsrSessionReference(snapshot);
  const auto* reference = std::get_if<csr::CsrTimingResult>(&computation.outcome);
  if (!reference)
    throw std::runtime_error("CSR CPU/full did not produce a timing result");
  requireNativeTimingEqual(snapshot.input, *reference, database);
}
}  // namespace ista
