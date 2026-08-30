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

#include <any>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "StaExecutionContract.hpp"

namespace ista::csr {
struct CsrSnapshotObservation;
struct CsrTimingResult;
struct StaAnalysisContext;
}  // namespace ista::csr

namespace ista {

class StaCsrSessionContext;
struct StaCsrReferenceAttempt;
struct StaCsrScatterAttempt;

class StaSessionOperations
{
 public:
  virtual ~StaSessionOperations() = default;
  virtual StaStatus initialize(std::uint64_t token, std::map<std::string, std::any>& config_map) = 0;
  virtual StaStatus runFullBaseline(std::uint64_t token, StaCsrSessionContext* capture_or_null) = 0;
  virtual StaStatus close(std::uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* released_out) = 0;
  virtual StaStatus copyObservation(std::uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) = 0;
  virtual StaCsrReferenceAttempt computeCsrReferenceForTesting(std::uint64_t token, StaCsrSessionContext* context) = 0;
  virtual StaCsrScatterAttempt scatterCsrTimingResultForTesting(std::uint64_t token, StaCsrSessionContext* context, const csr::CsrTimingResult& result) = 0;
};

class IncrementalStaSession
{
 public:
  explicit IncrementalStaSession(StaExecutionRequest request);
  IncrementalStaSession(StaExecutionRequest request, std::unique_ptr<StaSessionOperations> operations);
  IncrementalStaSession(const IncrementalStaSession&) = delete;
  IncrementalStaSession(IncrementalStaSession&&) = delete;
  ~IncrementalStaSession() noexcept;
  IncrementalStaSession& operator=(const IncrementalStaSession&) = delete;
  IncrementalStaSession& operator=(IncrementalStaSession&&) = delete;

  StaStatus initialize(const csr::StaAnalysisContext& analysis_context, std::map<std::string, std::any> config_map);
  StaExecutionResult runFullBaseline();
  StaStatus close();
  StaCsrReferenceAttempt computeCsrReferenceForTesting();
  StaCsrScatterAttempt scatterCsrTimingResultForTesting(const csr::CsrTimingResult& result);
  std::unique_ptr<csr::CsrSnapshotObservation> snapshotObservation();

  [[nodiscard]] StaSessionState state() const noexcept { return _state.load(std::memory_order_acquire); }
  [[nodiscard]] std::optional<std::uint64_t> sessionToken() const noexcept;
  [[nodiscard]] const StaExecutionRequest& request() const noexcept { return _request; }

 private:
  [[nodiscard]] StaStatus withCurrentState(StaStatus status) const;
  [[nodiscard]] StaStatus invalidState(std::string message) const;
  [[nodiscard]] bool isIncrementalRequest() const noexcept;
  [[nodiscard]] std::optional<std::string> observationOwnershipError(const csr::CsrSnapshotObservation& observation) const;

  StaExecutionRequest _request;
  std::unique_ptr<StaSessionOperations> _operations;
  std::atomic<std::uint64_t> _token{0};
  std::atomic<StaSessionState> _state{StaSessionState::kCreated};
  std::mutex _api_mutex;
  std::unique_ptr<StaCsrSessionContext> _context;
  std::unique_ptr<csr::CsrSnapshotObservation> _released_observation;
  std::optional<StaExecutionRecord> _resolved_record;
  bool _baseline_executed{false};
};

class StaSessionException : public std::runtime_error
{
 public:
  StaSessionException(StaStatus status, StaExecutionRequest request);

  [[nodiscard]] const StaStatus& status() const { return _status; }
  [[nodiscard]] const StaExecutionRequest& request() const { return _request; }

 private:
  StaStatus _status;
  StaExecutionRequest _request;
};

void throwOnStaFailure(const StaStatus& status, const StaExecutionRequest& request);
StaExecutionRecord requireStaExecutionRecord(StaExecutionResult result, const StaExecutionRequest& request);

}  // namespace ista
