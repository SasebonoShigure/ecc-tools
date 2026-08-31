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
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <vector>

#include "BufferTraceTypes.hpp"

namespace ista::buffer_trace {

[[nodiscard]] std::vector<std::uint8_t> canonicalSelectionProfileBytes(const SelectionProfile& profile);
[[nodiscard]] std::vector<std::uint8_t> canonicalBufferCandidateIdentityBytes(const BufferCandidateObservation& candidate);
[[nodiscard]] std::vector<std::uint8_t> canonicalReplayHeaderBytes(const ReplayHeader& header);
[[nodiscard]] std::vector<std::uint8_t> canonicalSyntheticArtifactRecipeBytes(const SyntheticArtifactRecipe& recipe);
[[nodiscard]] std::vector<std::uint8_t> canonicalSourceArtifactRefsBytes(const SourceArtifactRefs& artifacts);
[[nodiscard]] std::vector<std::uint8_t> canonicalReplayProgramBytes(const ReplayProgram& program);
[[nodiscard]] std::vector<std::uint8_t> canonicalGenerationReceiptBytes(const GenerationReceipt& receipt);

// This is the byte payload an integrity tool may identify. Functional replay
// equality must compare ReplayProgram and timing objects, never this digest.
[[nodiscard]] std::vector<std::uint8_t> canonicalBufferTraceIntegrityPayloadBytes(const BufferTraceArtifact& artifact);

}  // namespace ista::buffer_trace
