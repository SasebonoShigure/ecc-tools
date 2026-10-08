// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaBatchJson.hpp"
#include "CsrFingerprint.hpp"
#include "StrictJson.hpp"
#include "TimingEditJsonInternal.hpp"
#include "TimingEditValidation.hpp"
#include <array>
#include <limits>
#include <map>
#include <stdexcept>

namespace ista {
namespace {
using Json = nlohmann::json;
namespace wire = timing_edit::json_detail;
constexpr auto kSchema = "physical-sta-committed-batch/1.0.0";
void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
template<class Enum, std::size_t Size>
Enum parseToken(const Json& value, const std::array<Enum, Size>& choices)
{
  const auto name = wire::string(value, "execution token");
  for (auto choice : choices) if (toString(choice) == name) return choice;
  throw std::invalid_argument("unknown physical execution token");
}
Json executionJson(const StaExecutionRecord& value)
{
  return {{"requested_backend", toString(value.requested_backend)}, {"requested_update_strategy", toString(value.requested_update_strategy)},
      {"fallback_policy", toString(value.fallback_policy)}, {"executed_backend", toString(value.executed_backend)},
      {"executed_update_strategy", toString(value.executed_update_strategy)}, {"execution_kind", toString(value.execution_kind)},
      {"propagation_scope", toString(value.propagation_scope)}, {"incremental_batch_executed", value.incremental_batch_executed},
      {"fallback_to_full", value.fallback_to_full}, {"fallback_reason", value.fallback_reason ? Json(toString(*value.fallback_reason)) : Json(nullptr)}};
}
StaExecutionRecord executionValue(const Json& value)
{
  wire::exactObject(value, "execution", {"requested_backend", "requested_update_strategy", "fallback_policy", "executed_backend",
      "executed_update_strategy", "execution_kind", "propagation_scope", "incremental_batch_executed", "fallback_to_full", "fallback_reason"});
  StaExecutionRecord result{
      parseStaBackend(wire::string(value.at("requested_backend"), "requested_backend")),
      parseStaUpdateStrategy(wire::string(value.at("requested_update_strategy"), "requested_update_strategy")),
      parseStaFallbackPolicy(wire::string(value.at("fallback_policy"), "fallback_policy")),
      parseStaBackend(wire::string(value.at("executed_backend"), "executed_backend")),
      parseStaUpdateStrategy(wire::string(value.at("executed_update_strategy"), "executed_update_strategy")),
      parseToken(value.at("execution_kind"), std::array{StaExecutionKind::kFullUpdate, StaExecutionKind::kIncrementalUpdate, StaExecutionKind::kFallbackFull}),
      parseToken(value.at("propagation_scope"), std::array{StaPropagationScope::kFullGraph}),
      wire::boolean(value.at("incremental_batch_executed"), "incremental_batch_executed"),
      wire::boolean(value.at("fallback_to_full"), "fallback_to_full"), std::nullopt};
  // The physical data-only path permits only the existing level-conflict fallback.
  if (!value.at("fallback_reason").is_null())
    result.fallback_reason = parseToken(value.at("fallback_reason"), std::array{StaFallbackReason::kOldLevelOrderConflict});
  return result;
}
const char* orientationName(idb::IdbOrient value)
{
  switch (value) {
    case idb::IdbOrient::kN_R0: return "N";
    case idb::IdbOrient::kS_R180: return "S";
    case idb::IdbOrient::kW_R90: return "W";
    case idb::IdbOrient::kE_R270: return "E";
    case idb::IdbOrient::kFN_MY: return "FN";
    case idb::IdbOrient::kFS_MX: return "FS";
    case idb::IdbOrient::kFW_MX90: return "FW";
    case idb::IdbOrient::kFE_MY90: return "FE";
    default: throw std::invalid_argument("physical recovery orientation is invalid");
  }
}
idb::IdbOrient orientationValue(const Json& value)
{
  const auto name = wire::string(value, "orientation");
  for (auto orientation : {idb::IdbOrient::kN_R0, idb::IdbOrient::kS_R180, idb::IdbOrient::kW_R90, idb::IdbOrient::kE_R270,
                            idb::IdbOrient::kFN_MY, idb::IdbOrient::kFS_MX, idb::IdbOrient::kFW_MX90, idb::IdbOrient::kFE_MY90})
    if (name == orientationName(orientation)) return orientation;
  throw std::invalid_argument("physical recovery orientation is unknown");
}
void decodeEdits(const Json& value, timing_edit::EditBatch& batch, std::vector<PhysicalBufferLocation>& locations)
{
  require(value.at("edits").is_array(), "physical recovery edits must be an array");
  for (const auto& item : value.at("edits")) {
    wire::exactObject(item, "record.edits", {"edit", "physical"});
    auto edit = wire::edit(item.at("edit"), "record.edits.edit");
    const auto& physical = item.at("physical");
    wire::exactObject(physical, "physical", {"x_dbu", "y_dbu", "orientation"});
    locations.push_back({edit.creation_token, wire::i32(physical.at("x_dbu"), "x_dbu"),
                               wire::i32(physical.at("y_dbu"), "y_dbu"), orientationValue(physical.at("orientation"))});
    batch.edits.push_back(std::move(edit));
  }
}
void validateExecution(const StaExecutionRecord& execution, std::string_view levelization)
{
  require(!validateStaExecutionRecord(execution), "physical recovery execution is inconsistent");
  require(execution.executed_backend == execution.requested_backend
      && execution.execution_kind != StaExecutionKind::kBaselineOnly
      && (!execution.fallback_reason || execution.fallback_reason == StaFallbackReason::kOldLevelOrderConflict),
      "physical recovery cannot represent a baseline or backend fallback");
  const auto expected = execution.requested_backend == StaBackend::kCpu && execution.requested_update_strategy == StaUpdateStrategy::kFull
      ? "native_full" : execution.executed_update_strategy == StaUpdateStrategy::kIncremental ? "mixed" : "full";
  require(levelization == expected, "physical recovery levelization disagrees with actual execution");
}
std::map<timing_edit::CreationToken, const PhysicalBufferLocation*> validateBatch(std::string_view baseline_id,
    const timing_edit::EditBatch& batch, std::span<const PhysicalBufferLocation> locations,
    const StaExecutionRecord& execution, std::string_view levelization, std::int32_t dbu_per_um)
{
  require(csr::isLowercaseSha256(baseline_id), "physical recovery baseline identity is invalid");
  require(batch.expected_generation.value() < std::numeric_limits<std::uint64_t>::max(), "physical recovery generation exhausted");
  const auto errors = timing_edit::validateEditBatch(batch);
  if (!errors.empty()) throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  validateExecution(execution, levelization);
  require(dbu_per_um > 0 && locations.size() == batch.edits.size(), "physical recovery locations or DBU are invalid");
  std::map<timing_edit::CreationToken, const PhysicalBufferLocation*> positions;
  for (const auto& location : locations)
    require(positions.emplace(location.token, &location).second, "physical recovery repeats a location token");
  for (const auto& edit : batch.edits) {
    const auto position = positions.find(edit.creation_token);
    const auto* placement = std::get_if<timing_edit::PlacedInputs>(&edit.placement_inputs);
    const auto* rc = std::get_if<timing_edit::ParasiticRcInputs>(&edit.rc_inputs);
    require(position != positions.end() && placement && rc && rc->parasitic_policy == timing_edit::ParasiticEditPolicy::kPreRouteTree,
            "physical recovery requires located pre-route edits");
    const auto& location = *position->second;
    require(placement->x_microns == location.x_dbu / static_cast<double>(dbu_per_um)
        && placement->y_microns == location.y_dbu / static_cast<double>(dbu_per_um), "physical recovery DBU/micron coordinates differ");
    static_cast<void>(orientationName(location.orientation));
  }
  return positions;
}
}
std::pair<timing_edit::EditBatch, std::vector<PhysicalBufferLocation>>
decodePhysicalStaBatchRequest(std::string_view bytes)
{
  const auto value = json_detail::parseUniqueKeyJson(bytes);
  wire::exactObject(value, "physical batch", {"schema_version", "expected_generation", "edits"});
  require(wire::string(value.at("schema_version"), "schema_version") == "physical-sta-batch/1.0.0",
          "physical batch schema is unsupported");
  timing_edit::EditBatch batch{timing_edit::TransactionGeneration{wire::u64(value.at("expected_generation"), "expected_generation")}, {}};
  std::vector<PhysicalBufferLocation> locations;
  decodeEdits(value, batch, locations);
  const auto errors = timing_edit::validateEditBatch(batch);
  if (!errors.empty()) throw std::invalid_argument(errors.front().field + ": " + errors.front().message);
  return {std::move(batch), std::move(locations)};
}
std::string encodePhysicalStaRecoveryBatch(std::string_view baseline_id,
    const timing_edit::EditBatch& batch, std::span<const PhysicalBufferLocation> locations,
    const StaExecutionRecord& execution, std::string_view levelization, std::int32_t dbu_per_um)
{
  const auto positions = validateBatch(baseline_id, batch, locations, execution, levelization, dbu_per_um);
  Json edits = Json::array();
  for (const auto& edit : batch.edits) {
    const auto& location = *positions.at(edit.creation_token);
    edits.push_back({{"edit", wire::toJson(edit)}, {"physical", {{"x_dbu", location.x_dbu}, {"y_dbu", location.y_dbu},
        {"orientation", orientationName(location.orientation)}}}});
  }
  return Json{{"schema_version", kSchema}, {"baseline_id", baseline_id}, {"expected_generation", batch.expected_generation.value()},
      {"generation", batch.expected_generation.value() + 1}, {"execution", executionJson(execution)}, {"levelization", levelization},
      {"edits", std::move(edits)}}.dump();
}
PhysicalStaRecoveryBatch decodePhysicalStaRecoveryBatch(std::string_view bytes, std::string_view baseline_id,
    std::uint64_t expected_generation, const StaExecutionRequest& request, std::int32_t dbu_per_um)
{
  const auto value = json_detail::parseUniqueKeyJson(bytes);
  wire::exactObject(value, "record", {"schema_version", "baseline_id", "expected_generation", "generation", "execution", "levelization", "edits"});
  require(wire::string(value.at("schema_version"), "schema_version") == kSchema
      && wire::string(value.at("baseline_id"), "baseline_id") == baseline_id, "physical recovery schema or baseline differs");
  require(expected_generation < std::numeric_limits<std::uint64_t>::max()
      && wire::u64(value.at("expected_generation"), "expected_generation") == expected_generation
      && wire::u64(value.at("generation"), "generation") == expected_generation + 1, "physical recovery generation is not contiguous");
  PhysicalStaRecoveryBatch result{{timing_edit::TransactionGeneration{expected_generation}, {}}, {}, executionValue(value.at("execution")),
                                  wire::string(value.at("levelization"), "levelization")};
  require(StaExecutionRequest{result.execution.requested_backend, result.execution.requested_update_strategy,
                             result.execution.fallback_policy} == request, "physical recovery request differs");
  decodeEdits(value, result.batch, result.locations);
  static_cast<void>(validateBatch(baseline_id, result.batch, result.locations, result.execution, result.levelization, dbu_per_um));
  return result;
}
}  // namespace ista
