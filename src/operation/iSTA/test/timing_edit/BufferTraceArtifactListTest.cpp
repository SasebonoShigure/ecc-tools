// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include "BufferTraceCanonicalEncoding.hpp"
#include "BufferTraceJson.hpp"
#include "json.hpp"

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
}  // namespace ista::buffer_trace
