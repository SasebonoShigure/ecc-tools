// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <string>

#include "CsrRealDifferentialWire.hpp"
#include "CsrReferenceTypes.hpp"
#include "CsrSnapshotObservation.hpp"
#include "TimingResultScatter.hpp"

namespace ista::csr::real_diff {

struct EncodedSourceWire
{
  std::string wire_hex;
  std::string sha256;
};

[[nodiscard]] EncodedSourceWire encodeSnapshotCandidate(const CsrReferenceInput& input);
[[nodiscard]] NeutralWire encodeReferenceTiming(const CsrReferenceInput& input, const CsrTimingResult& result);
[[nodiscard]] Json identityToJson(const CsrSnapshotIdentity& identity);
[[nodiscard]] Json observationToJson(const CsrSnapshotObservation& observation);
[[nodiscard]] Json scatterToJson(const CsrTimingResultScatterAttempt& attempt);

}  // namespace ista::csr::real_diff
