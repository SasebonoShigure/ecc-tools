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

#include <string>
#include <string_view>
#include <variant>

#include "BufferTraceTypes.hpp"

namespace ista::buffer_trace {

using ReplayProgramJsonParseResult = std::variant<ReplayProgram, BufferTraceContractViolations>;
using BufferTraceArtifactJsonParseResult = std::variant<BufferTraceArtifact, BufferTraceContractViolations>;

// Both readers are closed-world and duplicate-key rejecting. The template
// reader is the only wire entry that accepts an empty workload batch list.
[[nodiscard]] ReplayProgramJsonParseResult parseReplayProgramJson(std::string_view json_text);
[[nodiscard]] ReplayProgramJsonParseResult parseReplayTemplateJson(std::string_view json_text);
[[nodiscard]] BufferTraceArtifactJsonParseResult parseBufferTraceArtifactJson(std::string_view json_text);

// Writers validate first, emit deterministic compact JSON, and never project
// generation evidence or integrity metadata into ReplayProgram.
[[nodiscard]] std::string replayProgramToJson(const ReplayProgram& program);
[[nodiscard]] std::string replayTemplateToJson(const ReplayProgram& program);
[[nodiscard]] std::string bufferTraceArtifactToJson(const BufferTraceArtifact& artifact);

}  // namespace ista::buffer_trace
