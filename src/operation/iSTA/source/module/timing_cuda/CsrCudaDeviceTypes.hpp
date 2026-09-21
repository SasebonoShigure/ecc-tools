#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace ista::csr::cuda_detail {

using Index = std::uint64_t;
inline constexpr Index absent = std::numeric_limits<Index>::max();
// These device codes are independent of runtime CUDA errors.
enum class Status : int { success, invariant, numeric, cycle };

template <class T> struct Value
{
  T value{};
  bool present{false};
};

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
};

struct Arc
{
  Index source{0};
  Index sink{0};
  Index child_begin{0};
  Index child_count{0};
  Index child_state_begin{0};
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

struct ClockLane
{
  Value<double> arrival, slew;
  Index predecessor{absent};
  double predecessor_delay{0.0};
  std::uint8_t predecessor_transition{2};
};

enum class SeedKind : std::uint8_t { none, clock, output, scalar };
struct SequentialSeed
{
  SeedKind kind{SeedKind::none};
  Index clock_node{absent}, path_start{absent}, path_tag{absent};
  Index child_begin{0}, child_count{0};
  double delay{0.0}, delay_min{0.0}, delay_max{0.0};
  std::uint8_t trigger{0};
};

struct NodeState
{
  double arrival{-std::numeric_limits<double>::infinity()};
  double required{std::numeric_limits<double>::infinity()};
  double slack{0.0};
  double launch{0.0};
  Index clock_name{absent};
  Index predecessor_node{absent};
  Index predecessor_arc{absent};
  Value<double> slew[4];
  Index clock_owner{absent};
  bool clock_point{false};
  Value<double> effective_arrival[4], effective_slew[4];
  ClockLane physical_clock[4];
  SequentialSeed sequential;
};

struct NodeSeed
{
  Index node{};
  NodeState state;
};

struct PathState
{
  double arrival{0.0};
  double slew{0.0};
  double launch{0.0};
  double predecessor_delay{0.0};
  Index start{0};
  Index predecessor_node{absent};
  Index predecessor_arc{absent};
  std::uint8_t predecessor_transition{2};
  Index crpr_node{absent};
  std::uint8_t crpr_transition{2};
  bool present{false};
};

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
  const Index *path_offsets{nullptr}, *path_tags{nullptr};
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
  NodeState* nodes{nullptr};
  PathState* paths{nullptr};
  ArcState* arc_state{nullptr};
  ChildState* child_state{nullptr};
  Index* indegrees{nullptr};
  Index* order{nullptr};
  Index* node_levels{nullptr};
  Index* level_offsets{nullptr};
  Index* level_offset_count{nullptr};
  Status* status{nullptr};
};

static_assert(std::is_trivially_copyable_v<DeviceView>);
static_assert(std::is_trivially_copyable_v<Arc>);
static_assert(std::is_trivially_copyable_v<NodeState>);

}  // namespace ista::csr::cuda_detail
