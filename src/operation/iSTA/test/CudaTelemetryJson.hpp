// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <stdexcept>
#include "CsrCudaBackend.hpp"
#include "json.hpp"

namespace ista::test {
inline nlohmann::json cudaReadbackJson(const csr::CsrCudaTelemetry& t)
{
  const auto classified = t.levelization_decision_bytes + t.level_metadata_d2h_bytes
      + t.status_d2h_bytes + t.candidate_d2h_bytes + t.result_d2h_bytes;
  if (classified > t.d2h_bytes)
    throw std::logic_error("CUDA readback categories exceed actual transfers");
  return {{"schema_version", "ista-cuda-readback/1.0.0"},
          {"stream_wait_ms", t.stream_wait_ms}, {"stream_waits", t.stream_waits},
          {"levelization_decisions", {{"bytes", t.levelization_decision_bytes}, {"wait_ms", t.levelization_decision_wait_ms}}},
          {"level_metadata", {{"bytes", t.level_metadata_d2h_bytes}, {"wait_ms", t.level_metadata_wait_ms}}},
          {"status", {{"bytes", t.status_d2h_bytes}, {"wait_ms", t.status_wait_ms}}},
          {"candidate", {{"bytes", t.candidate_d2h_bytes}, {"wait_ms", t.candidate_wait_ms}}},
          {"result", {{"bytes", t.result_d2h_bytes}, {"wait_ms", t.result_wait_ms}}},
          {"other_d2h_bytes", t.d2h_bytes - classified}};
}
}  // namespace ista::test
