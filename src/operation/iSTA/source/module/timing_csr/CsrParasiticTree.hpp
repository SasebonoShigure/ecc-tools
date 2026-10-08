// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <array>
#include <span>
#include <memory>
#include <string_view>
#include <vector>

#include "CsrParasiticModel.hpp"
#include "CsrReferenceNativeModels.hpp"

namespace ista::csr {

struct CsrRcPin
{
  NodeId node;
  std::string_view name;
  ArcOutputLoadMatrix capacitance{};
};

// Raw native parasitic values are already pF/Ohm. Pins are only those on this
// net, with normalized native pin/port capacitance for each analysis/transition.
// Borrowed names expire with this call. No timing answers or global graph scan.
// Rejects malformed/disconnected/mesh input; the caller retains scope rejection.
[[nodiscard]] CsrParasiticTree prepareCsrParasiticTree(
    const ParasiticNetModel& net, NodeId driver, std::span<const CsrRcPin> pins);

}  // namespace ista::csr
