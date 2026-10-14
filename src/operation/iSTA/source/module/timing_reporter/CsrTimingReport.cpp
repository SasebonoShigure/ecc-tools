// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingReporter.hpp"
#include "TimingAnalyzer.hpp"
#include "DataManager.hpp"
#include <sstream>

namespace ista {
std::vector<TimingPathGroup>& TimingReporter::pathGroups()
{
  return _groups ? *_groups : STADM.getDatabase().get_timing_path_group_list();
}

void TimingReporter::materializeReportPath(TimingPath& path)
{
  if (_analyzer) _analyzer->expandPathTrace(path);
  else TimingAnalyzer::materializePathTrace(path);
}

std::pair<std::string, std::string> TimingReporter::readCsrReport(csr::detail::CsrReportNodeReader& points, bool summary_only)
{
  TimingAnalyzer analyzer(points);
  TASummary summary;
  auto groups = analyzer.prepareReportGroups(summary);
  TimingReporter reporter(groups, analyzer);
  std::ostringstream qor, paths;
  qor.exceptions(std::ios::badbit | std::ios::failbit);
  paths.exceptions(std::ios::badbit | std::ios::failbit);
  reporter.outputQorSummaryReport(&qor);
  if (!summary_only) reporter.outputTimingPathsJson(&paths);
  return {std::move(qor).str(), std::move(paths).str()};
}
}  // namespace ista
