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

#include <unistd.h>

#include <cstdint>
#include <iostream>

#include "CsrRealDifferentialOracleEncoding.hpp"
#include "CsrRealDifferentialProbeSupport.hpp"
#include "CsrRealDifferentialScenario.hpp"

namespace ista::csr::real_diff {
namespace {

Json makeResult(const ProbeOptions& options, const ProbeExecution& execution)
{
  return {{"schema", "ista-csr-real-differential-probe/1.0.0"},
          {"lane", "oracle"},
          {"case", options.case_name},
          {"top", options.top},
          {"expected_clocks", options.expected_clocks},
          {"repeat_index", options.repeat_index},
          {"run_nonce", options.run_nonce},
          {"input_case_sha256", options.input_case_sha256},
          {"process_id", static_cast<std::uint64_t>(::getpid())},
          {"argv_receipt", makeArgvReceipt(options)},
          {"scenario", execution.scenario},
          {"timing", execution.payload},
          {"complete", true}};
}

}  // namespace
}  // namespace ista::csr::real_diff

int main(int argc, char* argv[])
{
  using namespace ista::csr::real_diff;
  try {
    const ProbeOptions options = parseProbeOptions(argc, argv, "oracle");
    const ProbeExecution execution = executeProbeScenario(options, [](const ista::Database& database, std::uint64_t token, const Json& scenario) {
      static_cast<void>(token);
      static_cast<void>(scenario);
      return neutralWireToJson(readAndEncodeNativeTiming(database));
    });
    writeProbeResultExclusive(options, makeResult(options, execution));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ista_csr_real_oracle_probe: " << error.what() << '\n';
    destroyStaInterfaceAfterFailure();
    return 1;
  }
}
