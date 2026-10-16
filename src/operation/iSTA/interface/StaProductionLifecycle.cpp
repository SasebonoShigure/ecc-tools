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
#include "StaProductionLifecycle.hpp"

#include <functional>
#include <utility>
#include <vector>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "ClockPropagator.hpp"
#include "DataManager.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "Logger.hpp"
#include "Monitor.hpp"
#include "PowerAnalyzer.hpp"
#include "PowerPropagator.hpp"
#include "PowerReporter.hpp"
#include "SDFWriter.hpp"
#include "TimingAnalyzer.hpp"
#include "TimingCharacterizer.hpp"
#include "TimingPropagator.hpp"
#include "TimingReporter.hpp"

namespace ista {
namespace {

std::optional<StaLifecycleStage> delayRuntimeStage(bool enabled)
{
  if (!enabled)
    return std::nullopt;
  return StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }};
}

void logStaHeader()
{
  // clang-format off
  STALOG.info(Loc::current(), ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
  STALOG.info(Loc::current(), "_____ _______________________       _______________________ ________ ________ ");
  STALOG.info(Loc::current(), "___(_)__  ___/___  __/___    |      __  ___/___  __/___    |___  __ \\___  __/");
  STALOG.info(Loc::current(), "__  / _____ \\ __  /   __  /| |      _____ \\ __  /   __  /| |__  /_/ /__  /  ");
  STALOG.info(Loc::current(), "_  /  ____/ / _  /    _  ___ |      ____/ / _  /    _  ___ |_  _, _/ _  /     ");
  STALOG.info(Loc::current(), "/_/   /____/  /_/     /_/  |_|      /____/  /_/     /_/  |_|/_/ |_|  /_/      ");
  STALOG.info(Loc::current(), ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
  // clang-format on
}

void logStaFooter()
{
  // clang-format off
  STALOG.info(Loc::current(), ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
  STALOG.info(Loc::current(), "_____ _______________________       _______________________   ________________________  __  ");
  STALOG.info(Loc::current(), "___(_)__  ___/___  __/___    |      ___  ____/____  _/___  | / /____  _/__  ___/___  / / /  ");
  STALOG.info(Loc::current(), "__  / _____ \\ __  /   __  /| |      __  /_     __  /  __   |/ /  __  /  _____ \\ __  /_/ / ");
  STALOG.info(Loc::current(), "_  /  ____/ / _  /    _  ___ |      _  __/    __/ /   _  /|  /  __/ /   ____/ / _  __  /    ");
  STALOG.info(Loc::current(), "/_/   /____/  /_/     /_/  |_|      /_/       /___/   /_/ |_/   /___/   /____/  /_/ /_/     ");
  STALOG.info(Loc::current(), ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
  // clang-format on
}

class DefaultStaProductionLifecycleOperations final : public StaProductionLifecycleOperations
{
 public:
  StaOperationResult initialize(std::map<std::string, std::any>& config_map) override { return initializeStaResources(config_map); }
  StaOperationResult runFullBaseline() override { return runFullStaPipeline(); }
  StaOperationResult extractLib() override { return runExtractLibPipeline(); }
  StaOperationResult updateTiming() override { return runUpdateTimingPipeline(); }
  StaOperationResult writeSdf(const std::string& file_path) override { return runWriteSdfPipeline(file_path); }
  StaOperationResult reportTiming() override { return runReportTimingPipeline(); }
  StaOperationResult close() override { return closeStaResources(); }
};

}  // namespace

StaProductionLifecycleOperations& defaultStaProductionLifecycleOperations()
{
  static DefaultStaProductionLifecycleOperations operations;
  return operations;
}

StaOperationResult initializeStaResources(std::map<std::string, std::any>& config_map)
{
  Monitor monitor;
  return executeStaPersistentInitialization(
      {{"Logger", [] { Logger::initInst(); },
        [] {
          logStaHeader();
          STALOG.printLogFilePath();
          STALOG.info(Loc::current(), "Starting...");
        },
        [] { Logger::destroyInst(); }},
       {"DataManager", [] { DataManager::initInst(); }, [&config_map] { STADM.input(config_map); }, [] { DataManager::destroyInst(); }},
       {"DelayCalculator", [] { DelayCalculator::initInst(); }, [&monitor] { STALOG.info(Loc::current(), "Completed", monitor.getStatsInfo()); },
        [] { DelayCalculator::destroyInst(); }}});
}

StaOperationResult runFullStaPipeline(bool include_artifacts)
{
  Monitor monitor;
  STALOG.info(Loc::current(), "Starting...");
  std::vector<StaLifecycleStage> stages{
      {"GraphBuilder", [] { GraphBuilder::initInst(); }, [] { STAGB.build(); }, [] { GraphBuilder::destroyInst(); }},
      {"ClockPropagator", [] { ClockPropagator::initInst(); }, [] { STACP.propagate(); }, [] { ClockPropagator::destroyInst(); }},
      {"TimingPropagator", [] { TimingPropagator::initInst(); }, [] { STATP.propagate(); }, [] { TimingPropagator::destroyInst(); }},
  };
  if (include_artifacts)
    stages.push_back({"PowerPropagator", [] { PowerPropagator::initInst(); }, [] { STAPP.propagate(); }, [] { PowerPropagator::destroyInst(); }});
  stages.push_back({"TimingAnalyzer", [] { TimingAnalyzer::initInst(); },
                    include_artifacts ? +[] { STATA.analyze(); } : +[] { STATA.propagateRequired(); },
                    [] { TimingAnalyzer::destroyInst(); }});
  if (include_artifacts) {
    stages.push_back({"PowerAnalyzer", [] { PowerAnalyzer::initInst(); }, [] { STAPA.analyze(); }, [] { PowerAnalyzer::destroyInst(); }});
    stages.push_back({"TimingReporter", [] { TimingReporter::initInst(); }, [] { STATR.report(); }, [] { TimingReporter::destroyInst(); }});
    stages.push_back({"PowerReporter", [] { PowerReporter::initInst(); }, [] { STAPR.report(); }, [] { PowerReporter::destroyInst(); }});
    stages.push_back({"SDFWriter", [] { SDFWriter::initInst(); }, [] { STASW.write(); }, [] { SDFWriter::destroyInst(); }});
  }
  StaOperationResult result = executeStaLifecyclePipeline(stages, delayRuntimeStage(true));
  if (result.succeeded)
    STALOG.info(Loc::current(), "Completed", monitor.getStatsInfo());
  return result;
}

StaOperationResult runExtractLibPipeline()
{
  Monitor monitor;
  STALOG.info(Loc::current(), "Starting...");
  std::vector<StaLifecycleStage> stages{
      {"GraphBuilder", [] { GraphBuilder::initInst(); }, [] { STAGB.build(); }, [] { GraphBuilder::destroyInst(); }},
      {"ClockPropagator", [] { ClockPropagator::initInst(); }, [] { STACP.propagate(); }, [] { ClockPropagator::destroyInst(); }},
      {"TimingPropagator", [] { TimingPropagator::initInst(); }, [] { STATP.propagate(); }, [] { TimingPropagator::destroyInst(); }},
      {"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyze(); }, [] { TimingAnalyzer::destroyInst(); }},
      {"TimingCharacterizer", [] { TimingCharacterizer::initInst(); }, [] { STATC.characterize(); }, [] { TimingCharacterizer::destroyInst(); }},
  };
  StaOperationResult result = executeStaLifecyclePipeline(stages, delayRuntimeStage(true));
  if (result.succeeded)
    STALOG.info(Loc::current(), "Completed", monitor.getStatsInfo());
  return result;
}

StaOperationResult runUpdateTimingPipeline()
{
  return executeStaLifecyclePipeline(
      {{"GraphBuilder", [] { GraphBuilder::initInst(); }, [] { STAGB.build(); }, [] { GraphBuilder::destroyInst(); }},
       {"ClockPropagator", [] { ClockPropagator::initInst(); }, [] { STACP.propagate(); }, [] { ClockPropagator::destroyInst(); }},
       {"TimingPropagator", [] { TimingPropagator::initInst(); }, [] { STATP.propagate(); }, [] { TimingPropagator::destroyInst(); }}},
      delayRuntimeStage(true), true);
}

StaOperationResult runWriteSdfPipeline(const std::string& file_path)
{
  return executeStaLifecyclePipeline({{"SDFWriter", [] { SDFWriter::initInst(); }, [&file_path] { STASW.write(file_path); }, [] { SDFWriter::destroyInst(); }}},
                                     delayRuntimeStage(false));
}

StaOperationResult runReportTimingPipeline()
{
  return executeStaLifecyclePipeline({{"TimingAnalyzer", [] { TimingAnalyzer::initInst(); }, [] { STATA.analyze(); }, [] { TimingAnalyzer::destroyInst(); }},
                                      {"TimingReporter", [] { TimingReporter::initInst(); }, [] { STATR.report(); }, [] { TimingReporter::destroyInst(); }}},
                                     delayRuntimeStage(false));
}

StaOperationResult closeStaResources()
{
  Monitor monitor;
  STALOG.info(Loc::current(), "Starting...");
  StaOperationResult result = executeStaCleanupSequence({{"DelayCalculator runtime", [] { STADC.destroy(); }},
                                                         {"DelayCalculator", [] { DelayCalculator::destroyInst(); }},
                                                         {"DataManager output", [] { STADM.output(); }},
                                                         {"DataManager", [] { DataManager::destroyInst(); }}});
  if (result.succeeded) {
    STALOG.info(Loc::current(), "Completed", monitor.getStatsInfo());
    STALOG.printLogFilePath();
    logStaFooter();
  }
  result.mergeCleanup(executeStaCleanupSequence({{"Logger", [] { Logger::destroyInst(); }}}));
#ifdef __GLIBC__
  malloc_trim(0);
#endif
  return result;
}

}  // namespace ista
