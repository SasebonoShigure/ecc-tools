// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <map>
#include <tuple>

#include "CsrReferenceNativeModels.hpp"
#include "CsrRcDriverCache.hpp"

namespace ista::csr {

// Native child identity, not interned table identity. Instance CTQ copies
// occupy a separate namespace from the Liberty children used by graph arcs.
using CsrCpuRcChildKey = std::tuple<NodeId, bool, std::uint64_t, std::size_t, AnalysisMode, Transition>;
using CsrCpuRcTimingKey = std::tuple<CsrCpuRcChildKey, double, bool>;
using CsrCpuRcDriverKey = std::tuple<NodeId, AnalysisMode, Transition, double, bool>;

struct CsrCpuRcCachedTiming
{
  CsrRcTimingRecord timing;
  const ChildTimingArcModel* child{nullptr};
  std::shared_ptr<const CsrParasiticTree> tree;
};

struct CsrCpuRcCache
{
  CsrCpuRcCache() = default;
  CsrCpuRcCache(const CsrCpuRcCache&) = delete;
  CsrCpuRcCache& operator=(const CsrCpuRcCache&) = delete;
  CsrCpuRcCache(CsrCpuRcCache&&) = default;
  CsrCpuRcCache& operator=(CsrCpuRcCache&&) = default;
  // CPU reference follows native associative cache semantics. Only actual
  // calls create entries; no fanout maps or path-tag Cartesian product.
  std::map<CsrCpuRcTimingKey, CsrCpuRcCachedTiming> timing;
  std::map<CsrCpuRcDriverKey, const CsrCpuRcCachedTiming*> arnoldi_drivers;
  std::map<CsrCpuRcDriverKey, const CsrCpuRcCachedTiming*> dmp_drivers;
};

// This performs live RC arithmetic, never consumes native timing answers.
// On a hit it must not re-publish a driver: native's last cache MISS wins.
[[nodiscard]] CsrRcEvaluationStatus evaluateCsrCpuRcChild(
    const CsrParasiticBinding& binding, const CsrCpuRcChildKey& key,
    const ChildTimingArcModel& child, double input_slew, CsrCpuRcCache& cache,
    double& delay, double& slew);

// Select native Arnoldi -> DMP -> ordinary NLDM for this child. The caller
// still controls delay-before-slew candidate order and table presence.
[[nodiscard]] CsrRcEvaluationStatus evaluateCsrCpuCellTiming(
    const CsrParasiticBinding& binding, const CsrCpuRcChildKey& key,
    const ChildTimingArcModel& child, double input_slew, double no_rc_load,
    bool slew, CsrCpuRcCache& cache, double& result);

// A sink reads only its terminal from the driver's last real cache miss.
// Unavailable lets the net caller continue input-port/raw-tree fallback.
[[nodiscard]] CsrRcEvaluationStatus evaluateCsrCpuRcLoad(
    const CsrParasiticBinding& binding, AnalysisMode analysis, Transition transition,
    double input_slew, const ReferencedLibraryCellModel* load_cell,
    const CsrCpuRcCache& cache, double& delay, double& slew);

// Null cache is native arc initialization: no driver lookup with NaN slew.
// Dynamic evaluation reads completed driver responses before port/tree fallback.
[[nodiscard]] CsrRcEvaluationStatus evaluateCsrCpuNetTiming(
    const CsrParasiticBinding& binding, AnalysisMode analysis, Transition transition,
    double input_slew, bool input_port, const ReferencedLibraryCellModel* load_cell,
    const InputPortRcThresholds& thresholds, const CsrCpuRcCache* cache,
    double& delay, double& slew);

}  // namespace ista::csr
