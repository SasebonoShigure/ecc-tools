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

#include <pybind11/pybind11.h>
#include <tcl_util.h>

#include <memory>

#include "IncrementalStaSession.hpp"

namespace python_interface {

bool initSTA(std::string& config, std::map<std::string, std::string>& config_dict);
bool runSTA();
bool extractLib();
bool destroySTA();
ista::StaExecutionRequest parseStaExecutionRequest(const std::map<std::string, std::string>& request);
pybind11::dict resolveStaExecution(const std::map<std::string, std::string>& request);
std::unique_ptr<ista::IncrementalStaSession> createIncrementalStaSession(const std::map<std::string, std::string>& request);
void initializeIncrementalStaSession(ista::IncrementalStaSession& session, pybind11::handle analysis_context, const std::string& config,
                                     std::map<std::string, std::string> config_dict);
pybind11::dict runIncrementalStaBaseline(ista::IncrementalStaSession& session);
void closeIncrementalStaSession(ista::IncrementalStaSession& session);
pybind11::object incrementalStaSessionToken(const ista::IncrementalStaSession& session);
pybind11::object incrementalStaSnapshotObservation(ista::IncrementalStaSession& session);

}  // namespace python_interface
