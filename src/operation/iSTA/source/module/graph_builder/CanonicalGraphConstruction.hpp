// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include "Arc.hpp"
#include "Database.hpp"
namespace ista::canonical_graph {
std::size_t appendArc(Database& database, Arc arc);
Arc makeNetArc(Database& database, const std::string& source, const std::string& sink, ArcType type, const std::string& owner);
bool appendLibraryCellArcs(Database& database, Instance& instance);
void refreshNetDriverLoad(Database& database, Net& net);
Arc makeArc(const std::string& source_pin, const std::string& sink_pin, ArcType type, const std::string& owner_name, const std::string& library_source_port,
            const std::string& library_sink_port, bool is_clock_arc, bool is_disable_arc, TimingCellArc* timing_cell_arc);
}  // namespace ista::canonical_graph
