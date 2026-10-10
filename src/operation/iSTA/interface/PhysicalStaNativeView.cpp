// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
#include "STAInterface.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista {
namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}

// NativeEditTransaction owns the derived graph/report undo. These raw maps
// must survive with their stable Net/Pin addresses until that undo is released.
struct RawViewUndo
{
  Database& database;
  std::map<std::string, Instance> instances;
  std::map<std::string, Pin> pins;
  std::map<std::string, Net> nets;
  std::map<std::string, ParasiticNet> parasitics;
  bool committed{false};
  explicit RawViewUndo(Database& database) : database(database) { swap(); }
  ~RawViewUndo() { if (!committed) swap(); }
  void swap() noexcept
  {
    instances.swap(database.get_instance_map());
    pins.swap(database.get_pin_map());
    nets.swap(database.get_net_map());
    parasitics.swap(database.get_parasitic_library().get_net_map());
  }
};

void restoreRawRc(Database& database, const csr::ParasiticModel& models)
{
  for (const auto& source : models.nets) {
    ParasiticNet net;
    net.set_net_name(source.embedded_name);
    net.set_lumped_capacitance(source.lumped_capacitance);
    for (const auto& value : source.nodes) {
      ParasiticNode node;
      node.set_node_name(value.embedded_name);
      node.set_capacitance(value.capacitance);
      node.set_x(value.x);
      node.set_y(value.y);
      require(net.get_node_map().emplace(value.map_key, std::move(node)).second, "native view repeats an RC node");
    }
    for (const auto& value : source.resistors) {
      auto& resistor = net.get_resistor_list().emplace_back();
      resistor.set_source_node(value.source);
      resistor.set_sink_node(value.sink);
      resistor.set_resistance(value.resistance);
    }
    require(database.get_parasitic_library().get_net_map().emplace(source.map_key, std::move(net)).second,
            "native view repeats an RC net");
  }
}


}  // namespace

void PhysicalStaSessionState::materializeCsr(Database& database, const timing_edit::NativeExecutionAuthority& authority,
    const timing_dual_csr::DualCsrState& graph, const csr::CsrTimingValues& values)
{
  require(authority.readyForQuery() && authority.sessionToken() == graph.session_token
              && &database == &STADM.getDatabase(), "native view requires the committed CSR owner");
  if (_native_generation == graph.generation) {
    auto prepared = csr::detail::prepareDualCsrTimingResult(database, graph, values);
    csr::detail::publishPreparedTimingResult(prepared);
  } else {
    auto derived = timing_edit::NativeEditTransaction::prepareFull(authority);
    RawViewUndo raw(database);
    auto& interface = STAInterface::getInst();
    interface.wrapInstanceList();
    interface.wrapPortList();
    interface.wrapNetList();
    STADM.buildInstanceList();
    STADM.buildNetList();
    STADM.buildInstanceTimingInfo();
    restoreRawRc(database, graph.reference_payload.parasitics);
    timing_edit::NativeCandidateExecutor::prepareFullGraph(derived);
    auto prepared = csr::detail::prepareDualCsrTimingResult(database, graph, values);
    csr::detail::publishPreparedTimingResult(prepared);
    derived.commit();
    raw.committed = true;
  }
  _native_generation = graph.generation;
  _timing_generation = graph.generation;
}
}  // namespace ista
