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
#include "py_ista.h"

#include <stdexcept>
#include <utility>

#include "STAInterface.hpp"

namespace python_interface {
bool initStaConfigMapByJSON(const std::string& config, std::map<std::string, std::any>& config_map);
void initStaConfigMapByDict(std::map<std::string, std::string>& config_dict, std::map<std::string, std::any>& config_map);
}  // namespace python_interface

namespace python_interface {
namespace py = pybind11;
namespace {

const std::string& requireRequestField(const std::map<std::string, std::string>& request, const char* field)
{
  auto found = request.find(field);
  if (found == request.end())
    throw std::invalid_argument(std::string("missing STA request field: ") + field);
  return found->second;
}

py::dict executionRecordToDict(const ista::StaExecutionRecord& record)
{
  py::dict result;
  result["requested_backend"] = ista::toString(record.requested_backend);
  result["requested_update_strategy"] = ista::toString(record.requested_update_strategy);
  result["fallback_policy"] = ista::toString(record.fallback_policy);
  result["executed_backend"] = ista::toString(record.executed_backend);
  result["executed_update_strategy"] = ista::toString(record.executed_update_strategy);
  result["execution_kind"] = ista::toString(record.execution_kind);
  result["propagation_scope"] = ista::toString(record.propagation_scope);
  result["incremental_batch_executed"] = record.incremental_batch_executed;
  result["fallback_to_full"] = record.fallback_to_full;
  result["fallback_reason"] = record.fallback_reason ? py::cast(ista::toString(*record.fallback_reason)) : py::none();
  return result;
}

std::map<std::string, std::any> parseStaConfig(const std::string& config, std::map<std::string, std::string>& config_dict)
{
  std::map<std::string, std::any> config_map;
  if (!config.empty() && !initStaConfigMapByJSON(config, config_map))
    throw std::invalid_argument("invalid STA JSON configuration");
  initStaConfigMapByDict(config_dict, config_map);
  return config_map;
}

}  // namespace

bool initStaConfigMapByJSON(const std::string& config, std::map<std::string, std::any>& config_map);
void initStaConfigMapByDict(std::map<std::string, std::string>& config_dict, std::map<std::string, std::any>& config_map);

bool initSTA(std::string& config, std::map<std::string, std::string>& config_dict)
{
  std::map<std::string, std::any> config_map;

  bool pass = false;
  pass = config.empty() ? true : initStaConfigMapByJSON(config, config_map);
  if (!pass) {
    return false;
  }
  initStaConfigMapByDict(config_dict, config_map);
  STAI.initSTA(config_map);
  return true;
}

bool runSTA()
{
  STAI.runSTA();
  return true;
}

bool extractLib()
{
  STAI.extractLib();
  return true;
}

bool destroySTA()
{
  STAI.destroySTA();
  return true;
}

ista::StaExecutionRequest parseStaExecutionRequest(const std::map<std::string, std::string>& request)
{
  if (request.size() != 3)
    throw std::invalid_argument("STA execution request must contain exactly backend, update_strategy, and fallback_policy");
  return {.backend = ista::parseStaBackend(requireRequestField(request, "backend")),
          .update_strategy = ista::parseStaUpdateStrategy(requireRequestField(request, "update_strategy")),
          .fallback_policy = ista::parseStaFallbackPolicy(requireRequestField(request, "fallback_policy"))};
}

py::dict resolveStaExecution(const std::map<std::string, std::string>& request)
{
  ista::StaExecutionRequest parsed = parseStaExecutionRequest(request);
  return executionRecordToDict(ista::requireStaExecutionRecord(ista::resolveStage1StaExecution(parsed), parsed));
}

std::unique_ptr<ista::IncrementalStaSession> createIncrementalStaSession(const std::map<std::string, std::string>& request)
{
  return std::make_unique<ista::IncrementalStaSession>(parseStaExecutionRequest(request));
}

void initializeIncrementalStaSession(ista::IncrementalStaSession& session, const std::string& config,
                                     std::map<std::string, std::string> config_dict)
{
  ista::throwOnStaFailure(session.initialize(parseStaConfig(config, config_dict)), session.request());
}

py::dict runIncrementalStaBaseline(ista::IncrementalStaSession& session)
{
  return executionRecordToDict(ista::requireStaExecutionRecord(session.runFullBaseline(), session.request()));
}

void closeIncrementalStaSession(ista::IncrementalStaSession& session)
{
  ista::throwOnStaFailure(session.close(), session.request());
}

}  // namespace python_interface
