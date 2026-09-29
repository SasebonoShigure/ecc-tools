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
#include <map>
#include <string>

#include "StaLifecycleGuard.hpp"

namespace ista {

class StaProductionLifecycleOperations
{
 public:
  virtual ~StaProductionLifecycleOperations() = default;
  virtual StaOperationResult initialize(std::map<std::string, std::any>& config_map) = 0;
  virtual StaOperationResult runFullBaseline() = 0;
  virtual StaOperationResult extractLib() = 0;
  virtual StaOperationResult updateTiming() = 0;
  virtual StaOperationResult writeSdf(const std::string& file_path) = 0;
  virtual StaOperationResult reportTiming() = 0;
  virtual StaOperationResult close() = 0;
};

StaProductionLifecycleOperations& defaultStaProductionLifecycleOperations();

StaOperationResult initializeStaResources(std::map<std::string, std::any>& config_map);
StaOperationResult runFullStaPipeline(bool include_artifacts = true);
StaOperationResult runExtractLibPipeline();
StaOperationResult runUpdateTimingPipeline();
StaOperationResult runWriteSdfPipeline(const std::string& file_path);
StaOperationResult runReportTimingPipeline();
StaOperationResult closeStaResources();

}  // namespace ista
