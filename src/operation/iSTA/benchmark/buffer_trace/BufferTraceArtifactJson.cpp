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
#include "BufferTraceJsonInternal.hpp"

namespace ista::buffer_trace::json_detail {
SourceArtifactRefs parseSources(const Json& sources)
{
  exactObject(sources, "replay.workload.artifacts",
              {"tech_lef", "cell_lefs", "liberties", "verilog", "sdc", "top", "analysis_context"});
  const auto source_path = [&](std::string_view role) {
    const std::string field = "replay.workload.artifacts." + std::string(role);
    exactObject(sources.at(std::string(role)), field, {"path"});
    return text(sources.at(std::string(role)).at("path"), field + ".path");
  };
  const auto& context = sources.at("analysis_context");
  exactObject(context, "replay.workload.artifacts.analysis_context", {"liberty_corner", "temperature", "rc_corner"});
  return {
      {source_path("tech_lef")},
      vectorOf<CellLefSourceRef>(sources.at("cell_lefs"), "replay.workload.artifacts.cell_lefs",
          [](const Json& item, std::string field) {
            exactObject(item, field, {"path"});
            return CellLefSourceRef{text(item.at("path"), field + ".path")};
          }),
      vectorOf<LibertySourceRef>(sources.at("liberties"), "replay.workload.artifacts.liberties",
          [](const Json& item, std::string field) {
            exactObject(item, field, {"path"});
            return LibertySourceRef{text(item.at("path"), field + ".path")};
          }),
      {source_path("verilog")}, {source_path("sdc")}, text(sources.at("top"), "replay.workload.artifacts.top"),
      {text(context.at("liberty_corner"), "analysis_context.liberty_corner"),
       text(context.at("temperature"), "analysis_context.temperature"),
       text(context.at("rc_corner"), "analysis_context.rc_corner")}};
}

Json sourcesToJson(const SourceArtifactRefs& sources)
{
  Json cell_lefs = Json::array();
  for (const auto& source : sources.cell_lefs)
    cell_lefs.push_back({{"path", source.path}});
  Json liberties = Json::array();
  for (const auto& source : sources.liberties)
    liberties.push_back({{"path", source.path}});
  const auto& context = sources.analysis_context;
  return {{"tech_lef", {{"path", sources.tech_lef.path}}},
          {"cell_lefs", std::move(cell_lefs)}, {"liberties", std::move(liberties)},
          {"verilog", {{"path", sources.verilog.path}}}, {"sdc", {{"path", sources.sdc.path}}},
          {"top", sources.top}, {"analysis_context", {{"liberty_corner", context.liberty_corner},
              {"temperature", context.temperature}, {"rc_corner", context.rc_corner}}}};
}
}  // namespace ista::buffer_trace::json_detail
