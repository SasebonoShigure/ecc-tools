#include "PreparedPhysicalBatch.hpp"
#include "PhysicalPlacementRows.hpp"
#include "PreRouteRcTestSupport.hpp"
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
  idb::IdbViaMaster via_master;
  std::vector<std::unique_ptr<idb::IdbVia>> port_vias;
  // Obstruction layers remain borrowed by the legacy obstruction list.
  std::unique_ptr<idb::IdbObsLayer> obstruction_owner;
  idb::IdbCellMaster* cell_master{};
  idb::IdbLayout layout;
  idb::IdbDesign design{&layout};
  std::array<izh::PhysicalBufferInsertion, 2> edits;
  Fixture()
  {
    via_master.set_name("physical_buffer_port_via_with_a_non_SSO_name");
    auto* layer = layout.get_layers()->set_layer("M1", "ROUTING");
    auto* master = layout.get_cell_master_list()->set_cell_master("BUF");
    cell_master = master;
    master->set_width(100); master->set_height(200); master->set_type(idb::CellMasterType::kCore);
    for (const auto output : {false, true}) {
      auto* term = master->add_term(output ? "Y" : "A");
      term->set_direction(output ? idb::IdbConnectDirection::kOutput : idb::IdbConnectDirection::kInput);
      term->set_type(idb::IdbConnectType::kSignal); term->set_average_position(output ? 80 : 20, output ? 140 : 60);
      term->set_bounding_box(output ? 75 : 15, output ? 130 : 50, output ? 85 : 25, output ? 150 : 70);
      auto* port = term->add_port();
      auto via = std::make_unique<idb::IdbVia>();
      via->set_instance_reference(&via_master); via->set_coordinate(output ? 80 : 20, output ? 140 : 60);
      port->add_via(via.get()); port_vias.push_back(std::move(via));
      auto* shape = port->add_layer_shape(); shape->set_layer(layer);
      shape->add_rect(output ? 75 : 15, output ? 130 : 50, output ? 85 : 25, output ? 150 : 70);
    }
    obstruction_owner.reset(master->add_obs()->add_obs_layer());
    auto* obstruction = obstruction_owner->get_shape();
    obstruction->set_layer(layer); obstruction->add_rect(30, 30, 70, 70);
    for (std::size_t n = 0; n < edits.size(); ++n) {
      auto* net = design.createOrFindNet("source" + std::to_string(n), idb::IdbConnectType::kSignal);
      auto* driver = design.createOrFindIoPin("driver" + std::to_string(n));
      driver->set_term()->set_direction(idb::IdbConnectDirection::kInput);
      require(design.connectPinToNet(driver, net), "driver connection failed");
      auto& edit = edits[n];
      edit.source_net = net; edit.master = master; edit.instance_name = "inserted_physical_buffer_" + std::to_string(n);
      edit.net_name = "buffered_physical_data_net_" + std::to_string(n); edit.input_pin = "A"; edit.output_pin = "Y";
      edit.x = 1000 + 200 * static_cast<int>(n); edit.y = 400; edit.orientation = idb::IdbOrient::kFS_MX;
      auto* io_sink = design.createOrFindIoPin("io_sink" + std::to_string(n));
      io_sink->set_term()->set_direction(idb::IdbConnectDirection::kOutput);
      require(design.connectPinToNet(io_sink, net), "IO sink connection failed");
      edit.sinks.push_back(io_sink);
      for (int i = 0; i < 40; ++i) {
        auto* instance = design.createInstance("sink" + std::to_string(n) + "_" + std::to_string(i), "BUF",
            idb::IdbInstanceType::kNetlist, idb::IdbPlacementStatus::kPlaced, idb::IdbOrient::kN_R0, 3000 + i * 200, 0);
        require(instance && design.connectPinToNet(instance->get_pin("A"), net), "sink connection failed");
        if (i < 35) edit.sinks.push_back(instance->get_pin("A"));
      }
    }
  }
  std::string state()
  {
    std::ostringstream out;
    for (auto* instance : design.get_instance_list()->get_instance_list()) {
      out << instance->get_name() << ':' << instance->get_id() << ':' << instance->get_coordinate()->get_x()
          << ',' << instance->get_coordinate()->get_y() << ':' << static_cast<int>(instance->get_orient()) << ';';
      for (auto* pin : instance->get_pin_list()->get_pin_list())
        out << pin->get_pin_name() << ':' << pin->get_net_name() << ':' << pin->get_net() << ',';
    }
    for (auto* pin : design.get_io_pin_list()->get_pin_list())
      out << pin->get_pin_name() << ':' << pin->get_net_name() << ':' << pin->get_net() << ',';
    for (auto* net : design.get_net_list()->get_net_list()) {
      out << net->get_net_name() << ':' << net->get_id() << ':';
      for (auto* pins : {net->get_io_pins(), net->get_instance_pin_list()})
        for (auto* pin : pins->get_pin_list()) out << pin << ',';
      for (auto* instance : net->get_instance_list()->get_instance_list()) out << instance << ',';
    }
    return out.str();
  }
  void validate()
  {
    require(design.validateConnectivity().ok, "physical connectivity is inconsistent");
    for (auto* net : design.get_net_list()->get_net_list())
      for (auto* pins : {net->get_io_pins(), net->get_instance_pin_list()})
        for (auto* pin : pins->get_pin_list()) require(pins->contains(pin), "pin index omitted a live pin");
  }
};
template<class F> void withoutAllocations(F&& action)
{
  ista_test_allocation::armCountOnly(); action();
  const auto count = ista_test_allocation::disable();
  require(count == 0, "physical apply/undo/commit allocated");
}
void placementRows()
{
  Fixture f;
  auto* site = f.layout.get_sites()->add_site_list("site");
  site->set_width(100); site->set_height(200);
  f.cell_master->set_site(site);
  auto* other_site = f.layout.get_sites()->add_site_list("same_size_different_site");
  other_site->set_width(100); other_site->set_height(200);
  for (int y : {0, 200, 400})
    f.layout.get_rows()->createRow("row" + std::to_string(y), site, 0, y,
        y == 400 ? idb::IdbOrient::kFS_MX : idb::IdbOrient::kN_R0, 120, 1, 100, 0);
  auto* box = f.layout.get_rows()->get_row_list().front()->get_bounding_box();
  const auto original_box = *box;
  for (bool overflow : {false, true}) {
    box->set_low_x(overflow ? std::numeric_limits<std::int32_t>::min() : 100);
    box->set_high_x(overflow ? std::numeric_limits<std::int32_t>::max() : 0);
    bool rejected = false;
    try { izh::PhysicalPlacementRows invalid(f.design); }
    catch (const std::invalid_argument&) { rejected = true; }
    *box = original_box;
    require(rejected, "invalid row rectangle reached native int32 extent arithmetic");
  }
  izh::PhysicalPlacementRows rows(f.design);
  const auto baseline = rows.geometry();
  for (int scenario = 0; scenario < 7; ++scenario) {
    auto edits = f.edits;
    switch (scenario) {
      case 0: edits[0].x += 1; break;
      case 1: edits[0].y += 1; break;
      case 2: edits[0].orientation = idb::IdbOrient::kN_R0; break;
      case 3: edits[1].x = edits[0].x; break;
      case 4: edits[0].x = 3000; edits[0].y = 0; edits[0].orientation = idb::IdbOrient::kN_R0; break;
      case 5: edits[0].x = 12000; break;
      case 6: f.cell_master->set_site(other_site); break;
    }
    bool rejected = false;
    try { izh::PreparedPhysicalBatch batch(f.design, edits); static_cast<void>(rows.prepare(batch)); }
    catch (const std::invalid_argument&) { rejected = true; }
    f.cell_master->set_site(site);
    require(rejected && rows.geometry() == baseline, "illegal placement changed row occupancy");
  }
  {
    izh::PreparedPhysicalBatch batch(f.design, f.edits);
    auto placement = rows.prepare(batch);
    require(batch.apply(), "placement abandon apply failed");
  }
  require(rows.geometry() == baseline, "abandoned placement changed row occupancy");
  {
    izh::PreparedPhysicalBatch batch(f.design, f.edits);
    auto placement = rows.prepare(batch);
    withoutAllocations([&] { require(batch.apply(), "placement commit apply failed"); batch.commit(); placement.commit(); });
  }
  require(rows.geometry() != baseline, "placement commit did not reserve occupied sites");
  for (auto& edit : f.edits) {
    edit.source_net = f.design.get_net_list()->find_net(edit.net_name);
    edit.instance_name += "_next"; edit.net_name += "_next";
  }
  bool rejected = false;
  try { izh::PreparedPhysicalBatch batch(f.design, f.edits); static_cast<void>(rows.prepare(batch)); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "next batch overlapped a committed buffer");
}
void run()
{
  placementRows();
  Fixture f; const auto before = f.state();
  const izh::PreRouteRcTechnology rc{1000, {"M1", 0.2, 1.0, 0.002, 0.001}, {"M2", 0.4, 0.5, 0.004, 0.001}};
  {
    izh::PreparedPhysicalBatch prepared(f.design, f.edits);
    require(f.state() == before, "prepare changed design");
    izh::PreparedPhysicalBatch batch(std::move(prepared));
    const auto trees = batch.prepareRc(rc);
    require(trees.size() == f.edits.size() * 2 && f.state() == before, "RC preparation changed design or omitted a net");
    withoutAllocations([&] { require(batch.apply(), "apply failed"); }); f.validate();
    for (const auto& edit : f.edits) {
      auto* net = f.design.get_net_list()->find_net(edit.net_name);
      auto* instance = f.design.get_instance_list()->find_instance(edit.instance_name);
      require(instance && net && instance->get_pin("A")->get_net() == edit.source_net && instance->get_pin("Y")->get_net() == net,
              "buffer binding is wrong");
      for (auto* pin : edit.sinks)
        require(pin->get_net() == net && (pin->is_io_pin() ? net->has_io_pin(pin) && !edit.source_net->has_io_pin(pin) : net->has_instance_pin(pin) && !edit.source_net->has_instance_pin(pin)), "sink did not move");
      require(instance->get_pin("A")->get_average_coordinate()->get_x() == edit.x + 20
          && instance->get_pin("A")->get_average_coordinate()->get_y() == edit.y + 140, "pin geometry is wrong");
    }
    for (const auto& [name, tree] : trees)
      require(izh::test::rcValues(tree) == izh::test::rcValues(izh::buildPreRouteRcNet(*f.design.get_net_list()->find_net(name), rc)),
              "prepared RC differs from the applied physical connectivity/coordinates");
    for (const auto& edit : f.edits) {
      const auto& node = trees.at(edit.source_net->get_net_name()).get_node_map().at(edit.instance_name + ":A");
      require(node.get_x() == (edit.x + 20) / 1000.0 && node.get_y() == (edit.y + 140) / 1000.0,
              "RC used instance origin instead of oriented pin coordinates");
    }
    withoutAllocations([&] { batch.rollback(); });
  }
  require(f.state() == before, "rollback changed IDs, coordinates or connection order"); f.validate();
  { izh::PreparedPhysicalBatch batch(f.design, f.edits); require(batch.apply(), "abandon apply failed"); }
  require(f.state() == before, "destructor did not undo"); f.validate();
  for (int round = 0; round < 2; ++round) {
    izh::PreparedPhysicalBatch batch(f.design, f.edits);
    withoutAllocations([&] { require(batch.apply(), "commit apply failed"); batch.commit(); }); f.validate();
    for (auto& edit : f.edits) {
      edit.source_net = f.design.get_net_list()->find_net(edit.net_name);
      edit.instance_name += "_next"; edit.net_name += "_next"; edit.x += 1000;
    }
  }
  const auto committed = f.state(); f.edits[0].sinks.push_back(f.edits[0].sinks.front());
  try { izh::PreparedPhysicalBatch bad(f.design, f.edits); throw std::runtime_error("duplicate sink accepted"); }
  catch (const std::invalid_argument&) {}
  require(f.state() == committed, "rejected batch changed design");
#if ECC_INCREGPUSTA_DEBUG_CHECKS
  std::size_t allocations;
  {
    Fixture base; ista_test_allocation::armCountOnly();
    izh::PreparedPhysicalBatch batch(base.design, base.edits);
    allocations = ista_test_allocation::disable();
  }
  for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
    Fixture base; const auto snapshot = base.state(); bool failed = false;
    ista_test_allocation::armFailure(ordinal);
    try { izh::PreparedPhysicalBatch batch(base.design, base.edits); }
    catch (const std::bad_alloc&) { failed = true; }
    (void) ista_test_allocation::disable();
    require(failed && base.state() == snapshot, "allocation failure changed physical state"); base.validate();
  }
  std::cout << "physical preparation allocation failure ordinals: " << allocations << '\n';
#endif
}
}
int main()
{
  try { run(); std::cout << "physical prepared batch contracts passed\n"; }
  catch (const std::exception& error) {
    (void) ista_test_allocation::disable(); std::cerr << error.what() << '\n'; return 1;
  }
}
