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
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "CsrRealDifferentialWire.hpp"

namespace ista::csr {
struct StaAnalysisContext;
}

namespace ista::csr::real_diff {

struct ProbeInput
{
  std::string label;
  std::filesystem::path path;
  std::string sha256;

  friend bool operator==(const ProbeInput&, const ProbeInput&) = default;
};

struct ProbeOptions
{
  std::string lane;
  std::filesystem::path executable;
  std::filesystem::path descriptor_manifest;
  std::filesystem::path tech_lef;
  std::filesystem::path cell_lef;
  std::filesystem::path liberty;
  std::filesystem::path verilog;
  std::filesystem::path sdc;
  std::vector<ProbeInput> inputs;
  std::string top;
  std::string case_name;
  std::vector<std::string> expected_clocks;
  std::string expected_clocks_json;
  std::uint64_t repeat_index{0};
  std::string input_case_sha256;
  std::string run_nonce;
  std::filesystem::path run_dir;
  std::string result_temp_name;
  std::filesystem::path result_temp;
  Json descriptor;
  std::vector<std::string> canonical_argv;
};

[[nodiscard]] ProbeOptions parseProbeOptions(int argc, char* argv[], std::string_view lane);
[[nodiscard]] Json makeArgvReceipt(const ProbeOptions& options);
[[nodiscard]] const StaAnalysisContext& probeAnalysisContext();
[[nodiscard]] std::map<std::string, std::any> makeProbeConfig(const ProbeOptions& options);
void loadProbeDesign(const ProbeOptions& options);

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path);
[[nodiscard]] std::string sha256File(const std::filesystem::path& path);
[[nodiscard]] std::string bytesToHex(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] std::vector<std::uint8_t> hexToBytes(std::string_view bytes, std::string_view label);

void writeProbeResultExclusive(const ProbeOptions& options, const Json& result);
void destroyStaInterfaceAfterFailure() noexcept;

}  // namespace ista::csr::real_diff
