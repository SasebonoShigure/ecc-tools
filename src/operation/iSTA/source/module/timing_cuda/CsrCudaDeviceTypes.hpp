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
  bool check_present[2]{};
};

struct Arc
{
  Index source{0};
  Index sink{0};
  Index child_begin{0};
  Index child_count{0};
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
  bool present{false};
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
};

// All storage is owned by one invocation's host runtime. No device allocation
// or pointers into STL objects. Optional values preserve absent versus zero.
struct DeviceView
{
  Index node_count{0};
  Index arc_count{0};
  Index child_count{0};
  Index tag_count{0};
  Index root_count{0};
  Index end_count{0};
  Index endpoint_count{0};
  const Index* out_offsets{nullptr};
  const Index* out_arcs{nullptr};
  Index aux_row_count{0};
  const Index* aux_offsets{nullptr};
  const Index* aux_arcs{nullptr};
  const Index* source_to_aux_row{nullptr};
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
