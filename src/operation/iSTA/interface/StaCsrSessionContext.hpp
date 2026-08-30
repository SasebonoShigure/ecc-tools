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

#include <cstdint>
#include <memory>
#include <variant>

#include "CsrReferenceTypes.hpp"
#include "TimingResultScatter.hpp"

namespace ista {

class Database;
class STAInterface;
class StaCsrSessionContextTestAccess;

namespace csr {
struct CsrCommittedSessionSnapshot;
struct CsrSnapshotObservation;
}  // namespace csr

enum class StaCsrSessionSubstate : std::uint8_t
{
  kAwaitingCapture,
  kCaptureFailed,
  kReady,
  kInvalidated,
  kReleased
};

enum class StaCsrSessionOperationCode : std::uint8_t
{
  kInvalidState
};

struct StaCsrSessionOperationFailure
{
  StaCsrSessionOperationCode code{StaCsrSessionOperationCode::kInvalidState};
  std::uint64_t writes_applied{0};
};

struct StaCsrCaptureAttempt
{
  std::variant<csr::CsrCaptureResult, StaCsrSessionOperationFailure> value;
};

struct StaCsrReferenceAttempt
{
  std::variant<csr::CsrComputationResult, StaCsrSessionOperationFailure> value;
};

struct StaCsrScatterAttempt
{
  csr::CsrTimingResultScatterAttempt value;
};

class StaCsrSessionContext final
{
 public:
  StaCsrSessionContext(csr::StaAnalysisContext context, std::uint64_t session_token);
  ~StaCsrSessionContext() noexcept;

  StaCsrSessionContext(const StaCsrSessionContext&) = delete;
  StaCsrSessionContext(StaCsrSessionContext&&) = delete;
  StaCsrSessionContext& operator=(const StaCsrSessionContext&) = delete;
  StaCsrSessionContext& operator=(StaCsrSessionContext&&) = delete;

  [[nodiscard]] const csr::StaAnalysisContext& analysisContext() const noexcept { return _analysis_context; }
  [[nodiscard]] std::uint64_t token() const noexcept { return _session_token; }

 private:
  friend class STAInterface;
  friend class StaCsrSessionContextTestAccess;

  [[nodiscard]] StaCsrSessionSubstate substate() const noexcept { return _substate; }
  [[nodiscard]] StaCsrCaptureAttempt capture(const Database& database);
  [[nodiscard]] StaCsrReferenceAttempt computeReference();
  [[nodiscard]] StaCsrScatterAttempt scatter(Database& database, const csr::CsrTimingResult& result);
  void release(std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) noexcept;
  [[nodiscard]] bool copyObservation(std::unique_ptr<csr::CsrSnapshotObservation>* detached_out);

  const csr::StaAnalysisContext _analysis_context;
  const std::uint64_t _session_token;
  StaCsrSessionSubstate _substate{StaCsrSessionSubstate::kAwaitingCapture};
  std::unique_ptr<csr::CsrCommittedSessionSnapshot> _snapshot;
};

}  // namespace ista
