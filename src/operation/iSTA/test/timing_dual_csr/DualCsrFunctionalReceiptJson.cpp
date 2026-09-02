// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrFunctionalReceiptJson.hpp"

namespace ista::timing_dual_csr::test {

nlohmann::json functionalSemanticReceiptToJson(
    const FunctionalSemanticReceipt& receipt)
{
  return {{"graph", detail::functionalGraphToJson(receipt.graph)},
          {"timing", detail::functionalTimingToJson(receipt.timing)}};
}

}  // namespace ista::timing_dual_csr::test
