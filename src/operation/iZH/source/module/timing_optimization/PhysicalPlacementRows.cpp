// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalPlacementRows.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace izh {
namespace {
using Interval = std::array<std::int32_t, 2>;
void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
std::array<std::int32_t, 2> extent(idb::IdbRect* box)
{
  require(box != nullptr, "physical placement rectangle is absent");
  const auto width = std::int64_t{box->get_high_x()} - box->get_low_x();
  const auto height = std::int64_t{box->get_high_y()} - box->get_low_y();
  require(width > 0 && height > 0 && width <= std::numeric_limits<std::int32_t>::max()
      && height <= std::numeric_limits<std::int32_t>::max(), "physical placement rectangle has invalid dimensions");
  return {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}
std::string orientation(idb::IdbOrient value)
{
  switch (value) {
    case idb::IdbOrient::kN_R0: return "N";
    case idb::IdbOrient::kFS_MX: return "FS";
    case idb::IdbOrient::kFN_MY: return "FN";
    case idb::IdbOrient::kS_R180: return "S";
    default: throw std::invalid_argument("physical buffer placement requires unrotated standard-cell rows");
  }
}
void merge(std::vector<Interval>& intervals)
{
  std::sort(intervals.begin(), intervals.end());
  std::size_t count = 0;
  for (const auto interval : intervals) {
    if (count && intervals[count - 1][1] >= interval[0])
      intervals[count - 1][1] = std::max(intervals[count - 1][1], interval[1]);
    else intervals[count++] = interval;
  }
  intervals.resize(count);
}
bool overlaps(const std::vector<Interval>& blocked, const Interval& value)
{
  const auto next = std::lower_bound(blocked.begin(), blocked.end(), value);
  return (next != blocked.end() && (*next)[0] < value[1])
      || (next != blocked.begin() && (*std::prev(next))[1] > value[0]);
}
}
PhysicalPlacementRows::PhysicalPlacementRows(idb::IdbDesign& design)
{
  auto* layout = design.get_layout();
  require(layout && layout->get_rows() && !layout->get_rows()->get_row_list().empty(),
          "physical buffer placement requires legal standard-cell rows");
  std::map<std::int32_t, std::vector<Interval>> allowed;
  bool first = true;
  for (auto* row : layout->get_rows()->get_row_list()) {
    require(row && row->get_site() && row->is_horizontal(), "physical placement row/site is invalid");
    const auto width = row->get_site()->get_width(), height = row->get_site()->get_height();
    auto* box = row->get_bounding_box();
    const auto size = extent(box);
    require(width > 0 && height > 0 && row->get_step_x() == width
        && size[0] % width == 0 && size[1] == height, "physical placement requires contiguous uniform row sites");
    if (first) {
      _site_width = width; _row_height = height; _site_name = row->get_site()->get_name();
      _bounds = {box->get_low_x(), box->get_low_y(), box->get_high_x(), box->get_high_y()};
      first = false;
    }
    require(!_site_name.empty() && row->get_site()->get_name() == _site_name
        && width == _site_width && height == _row_height
        && (std::int64_t{box->get_low_x()} - _bounds[0]) % width == 0
        && (std::int64_t{box->get_low_y()} - _bounds[1]) % height == 0,
        "physical placement rows do not share one site grid");
    auto [entry, fresh] = _rows.try_emplace(box->get_low_y(), Row{orientation(row->get_orient()), {}});
    require(fresh || entry->second.orientation == orientation(row->get_orient()), "row segments disagree on orientation");
    allowed[entry->first].push_back({box->get_low_x(), box->get_high_x()});
    _bounds[0] = std::min(_bounds[0], box->get_low_x()); _bounds[1] = std::min(_bounds[1], box->get_low_y());
    _bounds[2] = std::max(_bounds[2], box->get_high_x()); _bounds[3] = std::max(_bounds[3], box->get_high_y());
  }
  require(std::int64_t{_bounds[2]} - _bounds[0] <= std::numeric_limits<std::int32_t>::max()
      && std::int64_t{_bounds[3]} - _bounds[1] <= std::numeric_limits<std::int32_t>::max(),
      "physical placement bounds exceed the legalizer coordinate range");
  // Row gaps are hard obstacles for the rectangular low-level legalizer.
  for (std::int64_t y = _bounds[1]; y < _bounds[3]; y += _row_height) {
    auto& row = _rows[static_cast<std::int32_t>(y)];
    auto& spans = allowed[static_cast<std::int32_t>(y)]; merge(spans);
    auto cursor = _bounds[0];
    for (const auto span : spans) {
      if (cursor < span[0]) row.blocked.push_back({cursor, span[0]});
      cursor = span[1];
    }
    if (cursor < _bounds[2]) row.blocked.push_back({cursor, _bounds[2]});
  }
  const auto obstacle = [&](std::int64_t xl, std::int64_t yl, std::int64_t xh, std::int64_t yh) {
    xl = std::max<std::int64_t>(xl, _bounds[0]); xh = std::min<std::int64_t>(xh, _bounds[2]);
    yl = std::max<std::int64_t>(yl, _bounds[1]); yh = std::min<std::int64_t>(yh, _bounds[3]);
    if (xl >= xh || yl >= yh) return;
    const auto first_y = std::max<std::int64_t>(_bounds[1], yl - _row_height + 1);
    for (auto row = _rows.lower_bound(static_cast<std::int32_t>(first_y)); row != _rows.end() && row->first < yh; ++row)
      row->second.blocked.push_back({static_cast<std::int32_t>(xl), static_cast<std::int32_t>(xh)});
  };
  for (auto* instance : design.get_instance_list()->get_instance_list()) {
    require(instance && instance->get_status() != idb::IdbPlacementStatus::kUnplaced
        && instance->get_status() != idb::IdbPlacementStatus::kNone, "physical baseline contains an unplaced instance");
    auto* box = instance->get_bounding_box();
    static_cast<void>(extent(box));
    auto* halo = instance->get_halo();
    obstacle(std::int64_t{box->get_low_x()} - (halo ? halo->get_extend_lef() : 0),
        std::int64_t{box->get_low_y()} - (halo ? halo->get_extend_bottom() : 0),
        std::int64_t{box->get_high_x()} + (halo ? halo->get_extend_right() : 0),
        std::int64_t{box->get_high_y()} + (halo ? halo->get_extend_top() : 0));
  }
  for (auto* blockage : design.get_blockage_list()->get_blockage_list())
    if (blockage->is_palcement_blockage())
      for (auto* box : blockage->get_rect_list())
        obstacle(box->get_low_x(), box->get_low_y(), box->get_high_x(), box->get_high_y());
  // Existing fence regions reserve their area from new, unassigned buffers.
  for (auto* region : design.get_region_list()->get_region_list())
    if (region->get_type() == idb::IdbRegionType::kFence)
      for (auto* box : region->get_boundary())
        obstacle(box->get_low_x(), box->get_low_y(), box->get_high_x(), box->get_high_y());
  for (auto& [y, row] : _rows) { (void)y; merge(row.blocked); }
}
void PhysicalPlacementRows::requireBufferMaster(idb::IdbCellMaster& master) const
{
  require(master.is_core() && master.get_site() && master.get_site()->get_name() == _site_name
      && master.get_width() > 0 && master.get_width() <= std::numeric_limits<std::int32_t>::max()
      && master.get_width() % _site_width == 0 && master.get_height() == static_cast<std::uint32_t>(_row_height),
      "physical buffer master does not fit this LEF SITE");
}
PhysicalPlacementRows::Prepared PhysicalPlacementRows::prepare(const PreparedPhysicalBatch& batch)
{
  Prepared result;
  std::map<Row*, std::vector<Interval>> local;
  for (const auto& instance : batch.instances()) {
    requireBufferMaster(*instance->get_cell_master());
    auto* box = instance->get_bounding_box();
    const auto size = extent(box);
    const auto found = _rows.find(box->get_low_y());
    require(found != _rows.end() && !found->second.orientation.empty()
        && orientation(instance->get_orient()) == found->second.orientation
        && size[1] == _row_height && size[0] % _site_width == 0
        && (std::int64_t{box->get_low_x()} - _bounds[0]) % _site_width == 0
        && box->get_low_x() >= _bounds[0] && box->get_high_x() <= _bounds[2],
        "physical buffer is not on a legal row/site/orientation");
    auto& row = found->second;
    const Interval interval{box->get_low_x(), box->get_high_x()};
    require(!overlaps(row.blocked, interval), "physical buffer overlaps committed placement or a blockage");
    local[&row].push_back(interval);
  }
  result.additions.reserve(batch.instances().size());
  for (auto& [row, intervals] : local) {
    std::sort(intervals.begin(), intervals.end());
    for (std::size_t i = 0; i < intervals.size(); ++i) {
      require(i == 0 || intervals[i - 1][1] <= intervals[i][0], "physical batch buffers overlap");
      result.additions.emplace_back(row, intervals[i]);
    }
    const auto limit = row->blocked.max_size();
    if (intervals.size() > limit - row->blocked.size()) throw std::length_error("physical row capacity exhausted");
    const auto needed = row->blocked.size() + intervals.size();
    if (needed > row->blocked.capacity()) {
      const auto capacity = row->blocked.capacity();
      const auto grown = capacity > limit / 2 ? limit : capacity * 2;
      row->blocked.reserve(std::max(needed, grown));
    }
  }
  return result;
}
void PhysicalPlacementRows::Prepared::commit() noexcept
{
  for (const auto& [row, interval] : additions)
    row->blocked.insert(std::lower_bound(row->blocked.begin(), row->blocked.end(), interval), interval);
  additions.clear();
}
PhysicalPlacementRows::Geometry PhysicalPlacementRows::geometry() const
{
  Geometry result{_bounds, _site_width, _row_height, {}, {}};
  for (const auto& [y, row] : _rows) {
    if (!row.orientation.empty()) result.rows.emplace_back(y, row.orientation);
    for (const auto interval : row.blocked) result.obstacles.push_back({interval[0], y, interval[1], y + _row_height});
  }
  return result;
}
}  // namespace izh
