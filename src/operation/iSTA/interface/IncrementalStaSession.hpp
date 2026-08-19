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
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "StaExecutionContract.hpp"

namespace ista {

class StaSessionOperations
{
 public:
  virtual ~StaSessionOperations() = default;
  virtual StaStatus initialize(uint64_t token, std::map<std::string, std::any>& config_map) = 0;
  virtual StaStatus runFullBaseline(uint64_t token) = 0;
  virtual StaStatus close(uint64_t token) = 0;
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

  StaStatus initialize(std::map<std::string, std::any> config_map);
  StaExecutionResult runFullBaseline();
  StaStatus close();

  [[nodiscard]] StaSessionState state() const { return _state; }
  [[nodiscard]] const StaExecutionRequest& request() const { return _request; }

 private:
  StaStatus withCurrentState(StaStatus status) const;

  StaExecutionRequest _request;
  std::unique_ptr<StaSessionOperations> _operations;
  uint64_t _token;
  StaSessionState _state = StaSessionState::kCreated;
  std::optional<StaExecutionRecord> _resolved_record;
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
