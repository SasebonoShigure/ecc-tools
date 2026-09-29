// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include "BufferTraceCanonicalEncoding.hpp"
#include "BufferTraceJson.hpp"
#include "json.hpp"
#include "StrictJson.hpp"

namespace ista::buffer_trace {
namespace {
void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}
}

void testArtifactLists(ReplayProgram program)
{
  auto& sources = std::get<RealReplayPayload>(program.workload).artifacts;
  // Same basename in distinct directories is valid and ordering is significant.
  sources.cell_lefs = {{"/pdk/a/cells.lef"}, {"/pdk/b/cells.lef"}};
  sources.liberties = {{"/pdk/a/cells.lib"}, {"/pdk/b/cells.lib"}};
  sources.analysis_context = {"ss_1p08", "125", "__no_rc_corner__"};
  require(validateReplayProgram(program).empty(), "ordered multi-library input was rejected");
  const auto encoded = replayProgramToJson(program);
  const auto parsed = parseReplayProgramJson(encoded);
  require(std::holds_alternative<ReplayProgram>(parsed) && std::get<ReplayProgram>(parsed) == program,
          "multi-library JSON roundtrip lost source order or context");
  const auto original = sources;
  const auto identity = canonicalSourceArtifactRefsBytes(sources);
  std::swap(sources.cell_lefs[0], sources.cell_lefs[1]);
  require(canonicalSourceArtifactRefsBytes(sources) != identity, "LEF loading order was absent from identity");
  sources = original;
  std::swap(sources.liberties[0], sources.liberties[1]);
  require(canonicalSourceArtifactRefsBytes(sources) != identity, "Liberty loading order was absent from identity");
  for (auto member : {&csr::StaAnalysisContext::liberty_corner, &csr::StaAnalysisContext::temperature,
                      &csr::StaAnalysisContext::rc_corner}) {
    sources = original;
    sources.analysis_context.*member = "changed";
    require(canonicalSourceArtifactRefsBytes(sources) != identity, "analysis context was absent from identity");
  }
  sources = original;
  sources.cell_lefs.clear();
  require(!validateReplayProgram(program).empty(), "empty LEF list was accepted");
  sources = original;
  sources.liberties.clear();
  require(!validateReplayProgram(program).empty(), "empty Liberty list was accepted");
  sources = original;
  sources.liberties[1] = sources.liberties[0];
  require(!validateReplayProgram(program).empty(), "duplicated source path was accepted");
  const auto wire = nlohmann::json::parse(encoded);
  for (const std::string field : {"analysis_context", "cell_lefs", "liberties"}) {
    auto broken = wire;
    broken["workload"]["artifacts"].erase(field);
    require(std::holds_alternative<BufferTraceContractViolations>(parseReplayProgramJson(broken.dump())),
            "missing real source field was accepted");
  }
  for (const std::string field : {"liberty_corner", "temperature", "rc_corner"}) {
    auto broken = wire;
    broken["workload"]["artifacts"]["analysis_context"].erase(field);
    require(std::holds_alternative<BufferTraceContractViolations>(parseReplayProgramJson(broken.dump())),
            "incomplete real analysis context was accepted");
  }
}
void testDirectValueCodecBoundaries(const BufferTraceArtifact& artifact)
{
  using Json = nlohmann::json;
  using ista::json_detail::parseUniqueKeyJson;
  Json objects = Json::array();
  for (unsigned index = 0; index < 10000; ++index)
    objects.push_back({{"same_key", index}, {"nested", {{"same_key", true}}}});
  require(parseUniqueKeyJson(objects.dump()) == objects, "large sibling array or nested keys changed");
  for (const std::string duplicate : {R"({"a":1,"a":2})", R"([{"a":{"b":1,"b":2}}])",
                                      R"({"a":1,"\u0061":2})"}) {
    bool rejected = false;
    try { static_cast<void>(parseUniqueKeyJson(duplicate)); }
    catch (const ista::json_detail::DuplicateJsonKey&) { rejected = true; }
    require(rejected, "duplicate JSON key was accepted");
    const auto result = parseReplayProgramJson(duplicate);
    const auto* errors = std::get_if<BufferTraceContractViolations>(&result);
    require(errors && !errors->empty() && errors->front().code == BufferTraceContractCode::kDuplicate,
            "duplicate replay key lost its typed error");
  }
  for (const std::string malformed : {R"([{"a":1},])", R"({"a":1} {})", R"({"a":"\uD800"})", "// comment\n{}"}) {
    bool rejected = false;
    try { static_cast<void>(parseUniqueKeyJson(malformed)); }
    catch (const Json::exception&) { rejected = true; }
    require(rejected, "malformed or commented JSON was accepted");
  }
  require(parseUniqueKeyJson("/* accepted by Stage 3 */ {}", true) == Json::object(),
          "Stage 3 comment policy changed");
  const Json original = Json::parse(bufferTraceArtifactToJson(artifact));
  const auto reject_wire = [&](const Json& wire, const char* message) {
    require(std::holds_alternative<BufferTraceContractViolations>(parseBufferTraceArtifactJson(wire.dump())), message);
  };
  for (const auto ordinal : {std::uint64_t{1}, std::uint64_t{2}, std::numeric_limits<std::uint64_t>::max()}) {
    auto wire = original;
    auto token = wire["generation_only"]["rounds"][0]["evaluations"][0]["accepted_creation_token"];
    token["batch_ordinal"] = ordinal;
    wire["generation_only"]["rounds"][0]["evaluations"][0]["driver"]
        = {{"kind", "created"}, {"creation_token", std::move(token)}, {"role", "output"}};
    reject_wire(wire, "same/future driver reference was accepted or expanded into synthetic history");
  }
  auto wire = original;
  auto previous_token = wire["generation_only"]["rounds"][0]["evaluations"][0]["accepted_creation_token"];
  wire["generation_only"]["rounds"][1]["evaluations"][0]["driver"]
      = {{"kind", "created"}, {"creation_token", previous_token}, {"role", "output"}};
  auto parsed = parseBufferTraceArtifactJson(wire.dump());
  const auto* valid = std::get_if<BufferTraceArtifact>(&parsed);
  require(valid != nullptr && Json::parse(bufferTraceArtifactToJson(*valid)) == wire,
          "actual earlier driver token did not round-trip");
  wire["generation_only"]["rounds"][1]["evaluations"][0]["driver"]["creation_token"]["case_id"] = "foreign";
  reject_wire(wire, "earlier driver with a foreign token identity was accepted");
  wire["generation_only"]["rounds"][1]["evaluations"][0]["driver"]["creation_token"] = previous_token;
  wire["generation_only"]["rounds"][1]["evaluations"][0]["driver"]["creation_token"]["accepted_ordinal"]
      = std::numeric_limits<std::uint64_t>::max();
  reject_wire(wire, "nonexistent earlier driver was accepted or expanded into synthetic history");
  wire = original;
  wire["generation_only"]["rounds"][0]["evaluations"][0]["driver"]["unexpected"] = true;
  reject_wire(wire, "unknown nested reference key was accepted");
  wire = original;
  wire["replay"]["workload"]["batches"][0]["expected_generation"] = 0.5;
  reject_wire(wire, "fractional generation was accepted");
  wire = original;
  wire["replay"]["workload"]["batches"][1]["baseline"]["design_name"] = "different";
  reject_wire(wire, "batch baseline identity mismatch was accepted");
  wire = original;
  wire["replay"]["workload"]["batches"][1]["edits"][0]["original_net"]["role"] = "input";
  reject_wire(wire, "unknown created net role was accepted");
  wire = original;
  wire["replay"]["workload"]["batches"][0]["edits"][0]["creation_token"]["schema_version"]
      = std::uint64_t{1} << 32;
  reject_wire(wire, "out-of-range token schema was accepted");
}

}  // namespace ista::buffer_trace
