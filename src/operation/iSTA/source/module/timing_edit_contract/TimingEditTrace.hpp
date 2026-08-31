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
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TimingEditValidation.hpp"

namespace ista::timing_edit {

inline constexpr auto kTimingEditTraceSchema = "timing-edit-trace/1.0.0";

enum class TraceWorkloadKind : std::uint8_t
{
  kSynthetic,
  kRealDesign
};

[[nodiscard]] std::string_view token(TraceWorkloadKind value);

struct SourceArtifactIdentity
{
  std::string artifact_role;
  std::string logical_name;
  std::string content_sha256;

  friend bool operator==(const SourceArtifactIdentity&, const SourceArtifactIdentity&) = default;
};

struct ReplayCommand
{
  std::string executable;
  std::vector<std::string> arguments;

  friend bool operator==(const ReplayCommand&, const ReplayCommand&) = default;
};

struct TimingEditTraceBatch
{
  std::uint64_t ordinal{0};
  CommittedEditIdentity expected_pre_identity;
  std::vector<BufferInsertionMutation> edits;
  PolicyReceipt policy_receipt;
  CommittedEditIdentity expected_post_identity;

  friend bool operator==(const TimingEditTraceBatch&, const TimingEditTraceBatch&) = default;
};

struct TimingEditTrace
{
  std::string schema_version{kTimingEditTraceSchema};
  TraceWorkloadKind workload_kind{TraceWorkloadKind::kSynthetic};
  std::uint64_t seed{0};
  std::string profile;
  BaselineSemanticIdentity baseline;
  std::vector<SourceArtifactIdentity> source_artifacts;
  std::vector<TimingEditTraceBatch> batches;
  ReplayCommand replay_command;

  friend bool operator==(const TimingEditTrace&, const TimingEditTrace&) = default;
};

using TimingEditTraceParseResult = std::variant<TimingEditTrace, TimingEditContractViolations>;

[[nodiscard]] TimingEditContractViolations validateTimingEditTrace(const TimingEditTrace& value);
[[nodiscard]] std::vector<std::uint8_t> canonicalTimingEditTraceBytes(const TimingEditTrace& value);

// The wire reader is closed-world: every object has an exact key set and any
// unknown field, unknown tag, unknown version, dense/native ID, or precomputed
// timing answer is rejected before a typed trace is returned.
[[nodiscard]] TimingEditTraceParseResult parseTimingEditTraceJson(std::string_view json_text);
[[nodiscard]] std::string timingEditTraceToJson(const TimingEditTrace& value);

}  // namespace ista::timing_edit
