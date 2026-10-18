// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingAnalyzer.hpp"
#include "DataManager.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista {
namespace {
csr::AnalysisMode mode(AnalysisType value) { return value == AnalysisType::kMax ? csr::AnalysisMode::kMax : csr::AnalysisMode::kMin; }
csr::PathSource source(PathSourceType value) { return value == PathSourceType::kInput ? csr::PathSource::kInput : csr::PathSource::kRegister; }
csr::Transition transition(TransType value)
{
  return value == TransType::kRise ? csr::Transition::kRise
      : value == TransType::kFall ? csr::Transition::kFall : csr::Transition::kNone;
}
void appendInstanceClock(Database& database, csr::detail::CsrReportNodeReader& reader,
                         const std::string& name, std::vector<csr::NodeId>& nodes)
{
  const auto& pin = database.get_pin_map().at(name);
  if (pin.get_is_port()) return;
  const auto instance = database.get_instance_map().find(pin.get_instance_name());
  if (instance != database.get_instance_map().end() && instance->second.get_is_sequential()
      && reader.contains(instance->second.get_clock_pin_name()))
    nodes.push_back(reader.nodeId(instance->second.get_clock_pin_name()));
}
}  // namespace

void TimingAnalyzer::prepareCsrEndpoints()
{
  auto& database = STADM.getDatabase();
  auto& reader = *_csr_points;
  std::vector<csr::NodeId> clocks;
  for (const auto& name : database.get_end_point_list()) appendInstanceClock(database, reader, name, clocks);
  reader.prepare({clocks, {}, {}, {}});

  std::vector<csr::NodeId> endpoints;
  std::vector<std::string*> names;
  for (auto& name : database.get_end_point_list())
    if (isConstrainedEndPoint(name)) { endpoints.push_back(reader.nodeId(name)); names.push_back(&name); }
  reader.prepare({{}, endpoints, {}, {}});

  std::vector<csr::NodeId> starts;
  for (const auto* name : names) {
    auto& point = reader.point(*name);
    for (auto& [analysis, sources] : point.get_path_state_map())
      for (auto& [source, transitions] : sources)
        for (auto& [trans, tags] : transitions)
          for (auto& [tag, state] : tags) {
            static_cast<void>(analysis); static_cast<void>(source); static_cast<void>(trans); static_cast<void>(tag);
            const auto& start = state.get_start_point();
            starts.push_back(reader.nodeId(start));
            appendInstanceClock(database, reader, start, starts);
          }
  }
  reader.prepare({starts, {}, {}, {}});

  std::vector<csr::CsrClockChainSeed> roots;
  const auto append = [&](const ClockPathRoot& root) {
    if (reader.contains(root.pin)) roots.push_back({reader.nodeId(root.pin), mode(root.analysis), transition(root.transition)});
  };
  for (auto* name : names) {
    auto& point = reader.point(*name);
    for (auto& [analysis, sources] : point.get_path_state_map())
      for (auto& [source, transitions] : sources)
        for (auto& [trans, tags] : transitions)
          for (auto& [tag, state] : tags) {
            static_cast<void>(source); static_cast<void>(trans); static_cast<void>(tag);
            if (auto pair = resolveCrprRoots(state, *name, analysis)) { append(pair->launch); append(pair->capture); }
          }
  }
  reader.prepare({{}, {}, roots, {}});
}

void TimingAnalyzer::prepareCsrPaths(std::span<TimingPath* const> paths)
{
  if (!_csr_points || paths.empty()) return;
  auto& reader = *_csr_points;
  std::vector<csr::CsrPathChainSeed> chains;
  std::vector<csr::CsrClockChainSeed> clocks;
  const auto append = [&](const ClockPathRoot& root) {
    if (reader.contains(root.pin)) clocks.push_back({reader.nodeId(root.pin), mode(root.analysis), transition(root.transition)});
  };
  for (auto* path : paths) {
    auto& start = path->get_start_point();
    auto& end = path->get_end_point();
    const auto analysis = path->get_analysis_type();
    const auto from = path->get_source_type();
    const auto trans = path->get_trans_type();
    chains.push_back({reader.nodeId(end), mode(analysis), source(from), transition(trans), reader.clockTag(getClockName(start))});
    auto& state = getPathState(reader.point(end), analysis, from, trans, start);
    if (auto pair = resolveCrprRoots(state, end, analysis)) { append(pair->launch); append(pair->capture); }
  }
  reader.prepare({{}, {}, clocks, chains});
  reader.preparePathTrace();
}
}  // namespace ista
