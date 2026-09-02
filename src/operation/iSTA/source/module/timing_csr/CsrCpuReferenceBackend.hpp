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
#pragma once

#include "ActiveCsrEdgeView.hpp"
#include "CsrReferenceTypes.hpp"
#include "TimingPayloadView.hpp"

namespace ista::csr {
struct CsrCpuReferencePhaseTimings
{
  double levelization_ms{0.0};
  double propagation_ms{0.0};
};

struct CsrComputationWithTelemetry
{
  CsrComputationResult computation;
  CsrTraversalTelemetry telemetry;
  CsrCpuReferencePhaseTimings phase_timings;
};


class CsrCpuReferenceBackend final
{
 public:
  [[nodiscard]] CsrComputationResult compute(const CsrReferenceInput& input) const;
  [[nodiscard]] CsrComputationResult compute(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads) const;
  [[nodiscard]] CsrComputationWithTelemetry computeWithTelemetry(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads) const;
  [[nodiscard]] CsrComputationWithTelemetry computeWithTelemetry(const CsrReferenceInput& input) const;
};

}  // namespace ista::csr
