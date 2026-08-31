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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "IndependentSemanticReplayer.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit::oracle {

struct RawInstanceObservation
{
  std::string map_name;
  std::uint64_t instance_id{0};
  std::string instance_name;
  std::string cell_name;
  std::vector<std::string> pin_names;
  std::string clock_pin;
  std::string output_pin;
  std::string data_pin;
  bool is_sequential{false};
  bool is_clock_gating{false};
  bool has_clear{false};
  bool has_preset{false};
  bool is_io_cell{false};

  friend bool operator==(const RawInstanceObservation&, const RawInstanceObservation&) = default;
};

struct RawPinObservation
{
  std::string map_name;
  std::string pin_name;
  std::string full_name;
  std::string instance_name;
  std::string net_name;
  std::int32_t direction{0};
  double x{0.0};
  double y{0.0};
  bool is_port{false};

  friend bool operator==(const RawPinObservation&, const RawPinObservation&) = default;
};

struct RawNetObservation
{
  std::string map_name;
  std::string net_name;
  std::string driver_pin;
  std::vector<std::string> driver_pins;
  std::vector<std::string> load_pins;
  std::vector<std::string> pin_names;

  friend bool operator==(const RawNetObservation&, const RawNetObservation&) = default;
};

struct RawDesignObservation
{
  std::string design_name;
  std::vector<RawInstanceObservation> instances;
  std::vector<RawPinObservation> pins;
  std::vector<RawNetObservation> nets;

  friend bool operator==(const RawDesignObservation&, const RawDesignObservation&) = default;
};

struct IndependentOracleObservation
{
  RawDesignObservation raw;
  std::vector<std::string> derived_field_records;
};

[[nodiscard]] RawDesignObservation captureRawDesign(Database& database);
[[nodiscard]] RawDesignObservation projectExpectedRawAfterPrefix(const RawDesignObservation& baseline,
                                                                  const TimingEditTrace& trace,
                                                                  std::size_t prefix_batches);
[[nodiscard]] std::vector<std::string> compareRawDesign(const RawDesignObservation& expected,
                                                        const RawDesignObservation& observed);
[[nodiscard]] std::vector<std::string> validateDerivedDatabase(const Database& database,
                                                               const IndependentReplayReceipt& receipt);
[[nodiscard]] IndependentOracleObservation captureOracleObservation(Database& database,
                                                                    const IndependentReplayReceipt& receipt);
[[nodiscard]] std::string canonicalOracleObservationText(const IndependentOracleObservation& observation);

}  // namespace ista::timing_edit::oracle
