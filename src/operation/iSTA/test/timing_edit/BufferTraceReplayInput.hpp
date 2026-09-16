// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "BufferTraceJson.hpp"

namespace ista::buffer_trace::test {
// Functional replay has no selector receipt. Artifact input still validates it.
inline ReplayProgram readReplayInput(const std::filesystem::path& path, bool replay_only)
{
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open replay input: " + path.string());
  const std::string text(std::istreambuf_iterator<char>(input), {});
  if (input.bad())
    throw std::runtime_error("cannot read replay input: " + path.string());
  if (replay_only) {
    auto decoded = parseReplayProgramJson(text);
    if (auto* program = std::get_if<ReplayProgram>(&decoded))
      return std::move(*program);
    const auto& errors = std::get<BufferTraceContractViolations>(decoded);
    throw std::runtime_error("invalid replay program: " + errors.front().field + ": " + errors.front().message);
  }
  auto decoded = parseBufferTraceArtifactJson(text);
  if (auto* artifact = std::get_if<BufferTraceArtifact>(&decoded))
    return std::move(artifact->replay);
  const auto& errors = std::get<BufferTraceContractViolations>(decoded);
  throw std::runtime_error("invalid trace artifact: " + errors.front().field + ": " + errors.front().message);
}
}  // namespace ista::buffer_trace::test
