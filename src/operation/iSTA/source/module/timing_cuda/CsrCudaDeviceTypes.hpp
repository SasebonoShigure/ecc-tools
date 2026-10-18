#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "CsrQueryValues.hpp"

namespace ista::csr {
struct CsrRcTimingRow;
struct CsrRcTimingRecord;
struct CsrRcDriverLane;
}

namespace ista::csr::cuda_detail {

using Index = std::uint64_t;
inline constexpr Index absent = std::numeric_limits<Index>::max();
// These device codes are independent of runtime CUDA errors.
enum class Status : int { success, invariant, numeric, cycle };

struct PathRow
{
  Index begin{}, end{};
  friend bool operator==(const PathRow&, const PathRow&) = default;
};
struct PathRowUpdate { Index row; PathRow before, after; };

using value_detail::Value;

struct Table
{
  Index axis_offset[2]{};
  Index axis_size[2]{};
  Index value_offset{0};
  Index value_size{0};
  std::uint8_t dimensions{0};
  std::uint8_t variable1{0};
  bool present{false};
};

struct Child
{
  Table delay[2];
  Table slew[2];
  double time_scale{1.0};
  double capacitance_scale{1.0};
  std::int32_t arc_index{0};
  std::uint8_t sense{0};
  std::uint8_t trigger{2};
  Table check[2];
  Index library{absent};
  double slew_derate{1.0};
  double lower[2]{0.3, 0.3}, upper[2]{0.7, 0.7};
  double output_threshold[2]{0.5, 0.5};
};

// One immutable reduced lane; terminal payload is shared by all arcs on a net.
// Offsets survive host/device allocation growth, unlike embedded host pointers.
struct RcModel
{
  Index terminal_begin{0}, terminal_count{0};
  int order{0};
  double diagonal[5]{}, off_diagonal[5]{};
  double total_capacitance{0.0}, sqrt_total_capacitance{0.0}, root_load{0.0};
  bool dmp_valid{false};
  double driver_capacitance{0.0}, resistance{0.0}, load_capacitance{0.0};
};

struct RcTerminal
{
  double projection[5]{};
  int dmp_order{0};
  double poles[2]{}, residues[2]{};
  double fallback_delay{0.0}, fallback_impulse{0.0};
  bool fallback_valid{false};
};

struct RcModelView
{
  const RcModel* model{nullptr};
  const RcTerminal* terminals{nullptr};  // All lanes; model owns the offset.
};

struct RcLoadCell
{
  Index library{absent};
  double slew_derate{1.0};
  double lower[2]{0.3, 0.3}, upper[2]{0.7, 0.7}, input_threshold[2]{0.5, 0.5};
};

struct RcBinding
{
  Index net{absent}, terminal{0}, child_begin{0}, load_cell{absent};
  // Keep every byte defined after a device field assignment: bindings are
  // copied as whole objects when their persistent allocation grows.
  Index input_port{0};
};

struct RcNet
{
  const RcModel* models{nullptr};
  const RcTerminal* terminals{nullptr};
  Index lanes[4]{};
  Index child_count{0};
  CsrRcTimingRow* rows{nullptr};
  CsrRcTimingRecord* records{nullptr};
  CsrRcDriverLane* drivers{nullptr};
  const Index* initialize_arcs{nullptr};
  Index initialize_count{0};
};

struct Arc
{
  Index source{0};
  Index sink{0};
  Index child_begin{0};
  Index child_count{0};
  Index child_state_begin{0};
  Index rc_binding{absent};
  double delay{0.0};
  double delay_min{0.0};
  double delay_max{0.0};
  double load[4]{};
  bool active{true};
  bool net{true};
  bool clock{false};
  bool has_load{false};
  bool levelization{false};
  bool data_arrival{false};
  bool data_slew{false};
  bool clock_eligible{false};
};

using value_detail::ClockLane;

enum class SeedKind : std::uint8_t { none, clock, output, scalar };
struct SequentialSeed
{
  SeedKind kind{SeedKind::none};
  Index clock_node{absent}, path_start{absent}, path_tag{absent};
  Index child_begin{0}, child_count{0};
  Index rc_binding{absent};
  double delay{0.0}, delay_min{0.0}, delay_max{0.0};
  double output_load[4]{};
  std::uint8_t trigger{0};
};

struct NodeState : value_detail::NodeValue
{
  Index clock_owner{absent};
  SequentialSeed sequential;
};

struct NodeSeed
{
  Index node{};
  NodeState state;
};

using value_detail::PathState;

struct PathSeed
{
  Index row{}, tag{};
  PathState state;
};

struct ArcState
{
  double delay{0.0};
  double delay_min{0.0};
  double delay_max{0.0};
  double transition[4]{};
  Value<double> io[8];
  Value<double> graph[8];
  Value<std::uint8_t> type[2]{{2, false}, {2, false}};
};

struct ChildState
{
  Value<double> delay[8];
  std::uint8_t fallback[8]{};
};

struct Endpoint
{
  Index node{0};
  double period{0.0};
  Value<double> output_delay;
  bool sequential_check{false};
  Index clock_node{absent}, child_begin{0}, child_count{0};
  double check_time{0.0};
  std::uint8_t clock_transition{0};
};

struct AuxRow
{
  const Index* arcs{nullptr};
  Index count{};
};

// All storage is owned by one invocation's host runtime. No device allocation
// or pointers into STL objects. Optional values preserve absent versus zero.
struct DeviceView
{
  Index node_count{0};
  Index arc_count{0};
  Index child_count{0};
  Index child_state_count{0};
  Index tag_count{0};
  Index clock_count{0};
  Index path_count{0};
  const PathRow* path_rows{nullptr};
  const Index* path_tags{nullptr};
  Index root_count{0};
  Index end_count{0};
  Index endpoint_count{0};
  const Index* out_offsets{nullptr};
  const Index* out_arcs{nullptr};
  const AuxRow* aux_rows{nullptr};
  Index* storage_visits{nullptr};
  const Index* in_offsets{nullptr};
  const Index* in_arcs{nullptr};
  const Index* roots{nullptr};
  const Index* ends{nullptr};
  const Endpoint* endpoints{nullptr};
  const Arc* arcs{nullptr};
  const Child* children{nullptr};
  const double* table_data{nullptr};
  RcNet* rc_nets{nullptr};
  Index rc_net_count{0};
  const Index* rc_net_by_node{nullptr};
  const RcBinding* rc_bindings{nullptr};
  const RcLoadCell* rc_load_cells{nullptr};
  double rc_port_derate{1.0}, rc_port_lower[2]{0.3, 0.3}, rc_port_upper[2]{0.7, 0.7};
  NodeState* nodes{nullptr};
  PathState* paths{nullptr};
  ArcState* arc_state{nullptr};
  ChildState* child_state{nullptr};
  Index* indegrees{nullptr};
  Index* order{nullptr};
  Index* node_levels{nullptr};
  Index* level_offsets{nullptr};
  Status* status{nullptr};
};

static_assert(std::is_trivially_copyable_v<DeviceView>);
static_assert(std::is_trivially_copyable_v<Arc>);
static_assert(std::is_trivially_copyable_v<NodeState>);

}  // namespace ista::csr::cuda_detail
