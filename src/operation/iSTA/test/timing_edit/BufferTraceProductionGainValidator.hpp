// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "BufferFixedTraceGenerator.hpp"

namespace ista::buffer_trace::test_support {

// Recomputes every accepted gain from the production-exported CSR Liberty
// closure.
void requireProductionGenerationGain(
    const BufferTraceGenerationResult& generated);

}  // namespace ista::buffer_trace::test_support
