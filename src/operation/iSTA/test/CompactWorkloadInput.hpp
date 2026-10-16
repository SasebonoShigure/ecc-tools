// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <algorithm>
#include <array>
#include <fstream>
#include "Database.hpp"
#include "BufferTraceJsonInternal.hpp"
#include "BufferTraceValidationRules.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "StrictJson.hpp"
#include "TimingEditJsonInternal.hpp"

namespace ista::test {
struct CompactWorkloadInput
{
  buffer_trace::ReplayHeader header;
  buffer_trace::SourceArtifactRefs sources;
  std::vector<timing_edit::EditBatch> batches;
  std::vector<timing_edit::oracle::FreshArtifactEvidence> files;
  std::uint64_t instances{}, nodes{};
};

inline CompactWorkloadInput readCompactWorkload(const std::filesystem::path& path)
{
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open compact workload");
  const std::string text(std::istreambuf_iterator<char>(input), {});
  if (input.bad())
    throw std::runtime_error("cannot read compact workload");
  const auto json = json_detail::parseUniqueKeyJson(text);
  namespace wire = timing_edit::json_detail;
  wire::exactObject(json, "input", {"schema_version", "profile_id", "artifacts", "input_files", "batches",
                                   "expected_baseline_instances", "expected_baseline_nodes"});
  if (wire::string(json.at("schema_version"), "schema_version") != "ista-scale-compact-replay/2")
    throw std::invalid_argument("unsupported compact workload schema");
  CompactWorkloadInput result;
  result.header.workload_kind = buffer_trace::SemanticWorkloadKind::kRealDesign;
  const auto profile = wire::string(json.at("profile_id"), "profile_id");
  const auto profiles = std::array{buffer_trace::ProfileId::kEngineeringSmoke,
                                  buffer_trace::ProfileId::kPaperProtocolTop100X10,
                                  buffer_trace::ProfileId::kPaperProtocolTop1000X1};
  const auto selected = std::ranges::find_if(profiles, [&](auto id) { return buffer_trace::token(id) == profile; });
  if (selected == profiles.end())
    throw std::invalid_argument("unknown workload profile");
  result.header.profile_id = *selected;
  result.sources = buffer_trace::json_detail::parseSources(json.at("artifacts"));
  result.instances = wire::u64(json.at("expected_baseline_instances"), "instances");
  result.nodes = wire::u64(json.at("expected_baseline_nodes"), "nodes");
  if (!json.at("input_files").is_array() || !json.at("batches").is_array())
    throw std::invalid_argument("input_files and batches must be arrays");
  for (const auto& file : json.at("input_files")) {
    wire::exactObject(file, "input_file", {"index", "role", "logical_name", "sha256"});
    if (wire::u64(file.at("index"), "index") != result.files.size())
      throw std::invalid_argument("source file order differs");
    result.files.push_back({wire::string(file.at("role"), "role"),
                            wire::string(file.at("logical_name"), "logical_name"),
                            wire::string(file.at("sha256"), "sha256")});
  }
  for (const auto& batch : json.at("batches")) {
    wire::exactObject(batch, "batch", {"expected_generation", "edits"});
    timing_edit::EditBatch plan;
    plan.expected_generation = timing_edit::TransactionGeneration{wire::u64(batch.at("expected_generation"), "generation")};
    if (!batch.at("edits").is_array())
      throw std::invalid_argument("edits must be an array");
    for (const auto& edit : batch.at("edits"))
      plan.edits.push_back(wire::edit(edit, "edit"));
    result.batches.push_back(std::move(plan));
  }
  const auto errors = buffer_trace::detail::validateReplayBatches(result.header, result.batches);
  if (!errors.empty())
    throw std::invalid_argument("invalid workload: " + errors.front().field + ": " + errors.front().message);
  return result;
}

inline void requireWorkloadFiles(const CompactWorkloadInput& input,
                                 const timing_dual_csr::test::DualCsrTargetedArtifactSupport& artifacts)
{
  const auto actual = artifacts.capture();
  if (actual.size() != input.files.size())
    throw std::invalid_argument("source file count differs from frozen workload");
  for (std::size_t index = 0; index < actual.size(); ++index) {
    const auto& file = actual[index];
    if (timing_edit::oracle::FreshArtifactEvidence{file.role, file.logical_name, file.sha256} != input.files[index])
      throw std::invalid_argument("source file identity differs from frozen workload at index " + std::to_string(index));
  }
}

inline void requireWorkloadScale(const CompactWorkloadInput& input, const Database& database)
{
  if (database.get_instance_map().size() != input.instances || database.get_timing_point_map().size() != input.nodes)
    throw std::invalid_argument("actual baseline scale differs from frozen workload");
}

// CUDA has an actual baseline-binding consumer. Native execution never constructs this identity.
inline buffer_trace::ReplayProgram cudaWorkloadReplay(CompactWorkloadInput&& input,
    const timing_edit::oracle::ProductionFreshSessionView& view)
{
  auto identity = timing_edit::NativeTimingEditPreparer::identityFromInput(
      view.database, view.reference_input, timing_edit::TransactionGeneration{0});
  buffer_trace::RealReplayPayload payload;
  payload.artifacts = std::move(input.sources);
  payload.batches = std::move(input.batches);
  payload.baseline = {view.database.get_design_name(), std::move(identity.current_fingerprints),
                      std::move(identity.canonical_pre_state)};
  return {input.header, std::move(payload)};
}
}  // namespace ista::test
