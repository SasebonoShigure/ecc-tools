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

#include "TimingEditTrace.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit::oracle {

struct IndependentTokenNames
{
  CreationToken token;
  std::string instance;
  std::string output_net;
  std::string input_pin;
  std::string output_pin;

  friend bool operator==(const IndependentTokenNames&, const IndependentTokenNames&) = default;
};

struct ElectricalLoadObservation
{
  std::string output_pin;
  std::int32_t analysis{0};
  std::int32_t transition{0};
  double load{0.0};

  friend bool operator==(const ElectricalLoadObservation&, const ElectricalLoadObservation&) = default;
};

struct IndependentReplayReceipt
{
  std::size_t prefix_batches{0};
  std::vector<IndependentTokenNames> materialized_tokens;
  std::vector<ElectricalLoadObservation> electrical_loads;
};

// IndependentSemanticReplayer consumes only the public typed semantic
// contract. It derives names from CreationToken itself and edits raw
// Instance/Pin/Net state directly. It has no production prepared/resolved
// payload and no production edit implementation dependency.
class IndependentSemanticReplayer final
{
 public:
  [[nodiscard]] static IndependentReplayReceipt replayPlan(Database& database, const DesignEditPlan& plan);
  [[nodiscard]] static IndependentReplayReceipt replayPrefix(Database& database, const TimingEditTrace& trace,
                                                               std::size_t prefix_batches);
};

}  // namespace ista::timing_edit::oracle
