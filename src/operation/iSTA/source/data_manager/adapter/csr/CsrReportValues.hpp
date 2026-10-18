// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <map>
#include <span>
#include <functional>
#include <vector>
#include "CsrReferencePrimitives.hpp"
#include "CsrQueryValues.hpp"

namespace ista::csr {
struct CsrTimingValues;

struct CsrQueryPath
{
  value_detail::Index row{}, slot{}, tag{};
};

struct CsrClockChainSeed
{
  NodeId node;
  AnalysisMode analysis;
  Transition transition;
};
struct CsrPathChainSeed
{
  NodeId endpoint;
  AnalysisMode analysis;
  PathSource source;
  Transition transition;
  ClockTagId tag;
};
struct CsrReportRequest
{
  std::span<const NodeId> nodes, all_path_nodes;
  std::span<const CsrClockChainSeed> clock_chains;
  std::span<const CsrPathChainSeed> path_chains;
};
using PrepareCsrReport = std::function<std::vector<CsrQueryPath>(const CsrReportRequest&)>;

struct CsrQueryNode
{
  value_detail::NodeValue timing;
  value_detail::Index level{};
};

// Owned by one committed generation; physical path slots never cross commits.
struct CsrReportValues
{
  std::map<value_detail::Index, CsrQueryNode> nodes;
  std::map<value_detail::Index, value_detail::PathState> paths;
  const CsrTimingValues* complete{nullptr};
};
}  // namespace ista::csr
