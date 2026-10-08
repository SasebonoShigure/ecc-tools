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

#include <optional>

#include "CsrReferenceNativeModels.hpp"

namespace ista::csr {

[[nodiscard]] std::optional<double> lookupCsrTimingTable(const TimingTableModel& table, double input_slew, double raw_output_load, double time_unit_scale,
                                                         double capacitance_unit_scale);

// RC Ceff uses continuous raw load; the no-parasitic lookup retains its native ceil rule.
[[nodiscard]] std::optional<double> lookupCsrTimingTableRawLoad(const TimingTableModel& table, double input_slew, double raw_output_load,
                                                                double time_unit_scale, double capacitance_unit_scale);

[[nodiscard]] std::optional<double> lookupCsrTimingCheckTable(const TimingTableModel& table, double clock_slew, double data_slew,
                                                              double time_unit_scale);

}  // namespace ista::csr
