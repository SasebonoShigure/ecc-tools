// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2. This file is a regression test.
// ***************************************************************************************

#include "VerilogReader.hh"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void testEscapedIdentifierWithBusCharactersStaysScalar()
{
  const auto verilog_path = std::filesystem::temp_directory_path() / "verilog_escaped_identifier_test.v";
  {
    std::ofstream stream(verilog_path);
    require(stream.good(), "failed to create test Verilog file");
    stream << "module leaf(A);\n"
           << "  input A;\n"
           << "endmodule\n"
           << "module top;\n"
           << "  wire \\hierarchy[0].wdata_i[0] ;\n"
           << "  leaf \\hierarchy[0].instance ( .A(\\hierarchy[0].wdata_i[0] ) );\n"
           << "endmodule\n";
  }

  idb::VerilogReader reader;
  require(reader.readVerilog(verilog_path.c_str()) != 0, "failed to parse test Verilog file");

  ParsedVerilogModule* top_module = nullptr;
  for (auto* module : reader.get_verilog_modules()) {
    if (module != nullptr && std::string(module->module_name) == "top") {
      top_module = module;
      break;
    }
  }
  require(top_module != nullptr, "top module was not parsed");

  ParsedVerilogInst* instance = nullptr;
  void* statement = nullptr;
  FOREACH_VERILOG_VEC_ELEM(&top_module->module_stmts, void, statement)
  {
    if (verilog_is_module_inst_stmt(statement)) {
      instance = verilog_convert_inst(statement);
      break;
    }
  }
  require(instance != nullptr, "test instance was not parsed");
  require(instance->port_connections.len == 1, "test instance must have one connection");

  void* connection_handle = GetVerilogVecElem<void>(&instance->port_connections, 0);
  auto* connection = verilog_convert_port_ref_port_connect(connection_handle);
  require(connection != nullptr && verilog_is_id_expr(connection->net_expr), "escaped net must be an identifier expression");

  auto* expression = verilog_convert_net_id_expr(connection->net_expr);
  void* identifier = const_cast<void*>(expression->verilog_id);
  require(verilog_is_id(identifier), "escaped identifier with brackets must not become a bus index");
  require(std::string(verilog_convert_id(identifier)->id) == "\\hierarchy[0].wdata_i[0]", "escaped identifier text was not preserved");

  std::filesystem::remove(verilog_path);
}

void testStatementGrowthAndFlattenedView()
{
  const auto path = std::filesystem::temp_directory_path() / "verilog_statement_growth_test.v";
  {
    std::ofstream stream(path);
    require(stream.good(), "failed to create statement growth fixture");
    stream << "module child(A); input A; wire inner; assign inner = A;\n";
    for (int index = 0; index < 128; ++index) {
      stream << "BUF leaf" << index << " (.A(inner));\n";
    }
    stream << "endmodule\nmodule top(A); input A; child nested (.A(A));\n";
    for (int index = 0; index < 128; ++index) {
      stream << "BUF direct" << index << " (.A(A));\n";
    }
    stream << "endmodule\n";
  }
  idb::VerilogReader reader;
  require(reader.readVerilog(path.c_str()) != 0, "statement growth parse failed");
  ParsedVerilogModule* view = nullptr;
  for (auto* module : reader.get_verilog_modules()) {
    if (std::string(module->module_name) == "top") {
      view = module;
    }
  }
  require(view != nullptr && view->port_list.len == 1, "initial C module view is missing");
  const auto instances = [](ParsedVerilogModule* module) {
    std::vector<std::string> names;
    void* statement = nullptr;
    FOREACH_VERILOG_VEC_ELEM(&module->module_stmts, void, statement) {
      if (verilog_is_module_inst_stmt(statement)) {
        names.emplace_back(verilog_convert_inst(statement)->inst_name);
      }
    }
    return names;
  };
  std::vector<std::string> direct;
  for (int index = 0; index < 128; ++index) {
    direct.push_back("direct" + std::to_string(index));
  }
  auto initial = direct;
  initial.insert(initial.begin(), "nested");
  require(instances(view) == initial, "statement growth changed parse order");
  auto flattened = direct;
  for (int index = 0; index < 128; ++index) {
    flattened.push_back("nested/leaf" + std::to_string(index));
  }
  for (int repeat = 0; repeat < 2; ++repeat) {
    require(reader.flattenModule("top") != 0, "flatten failed");
    require(instances(view) == flattened, "borrowed C view lost append/erase order after flatten");
    require(view->port_list.len == 1, "flatten invalidated port handles");
    require(view->module_stmts.len == 259, "flatten lost declarations or assignment");
  }
  auto moved = std::move(reader);
  require(moved.get_top_module() == view && instances(view) == flattened, "move lost parser-owned views");
  idb::VerilogReader assigned;
  require(assigned.readVerilog(path.c_str()) != 0, "assignment target parse failed");
  assigned = std::move(moved);
  require(assigned.get_top_module() == view && instances(view) == flattened, "move assignment lost parser-owned views");
  require(assigned.readVerilog(path.c_str()) != 0, "reader reload failed");
  require(assigned.get_verilog_modules().size() == 2 && assigned.get_top_module() == nullptr, "reload kept stale module views");
  require(assigned.readVerilog((path.string() + ".missing").c_str()) == 0, "missing input unexpectedly parsed");
  require(assigned.get_verilog_modules().empty() && assigned.get_top_module() == nullptr, "failed reload kept stale views");
  require(reader.readVerilog(path.c_str()) != 0, "moved-from reader could not load another file");
  std::filesystem::remove(path);
}

}  // namespace

int main()
{
  try {
    testEscapedIdentifierWithBusCharactersStaysScalar();
    testStatementGrowthAndFlattenedView();
  } catch (const std::exception& error) {
    std::cout << error.what() << '\n';
    return 1;
  }
  return 0;
}
