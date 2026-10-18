// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <map>
#include "CsrQueryValues.hpp"

namespace ista::csr {
struct CsrTimingValues;

struct CsrQueryPath
{
  value_detail::Index row{}, slot{};
};

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
