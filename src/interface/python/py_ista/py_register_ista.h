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

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "py_ista.h"

namespace python_interface {
namespace py = pybind11;

inline PyObject* sta_session_exception_type = nullptr;

inline void translateStaSessionException(std::exception_ptr exception)
{
  try {
    if (exception)
      std::rethrow_exception(exception);
  } catch (const ista::StaSessionException& error) {
    py::object instance = py::reinterpret_steal<py::object>(PyObject_CallFunction(sta_session_exception_type, "s", error.what()));
    if (!instance)
      return;
    const ista::StaStatus& status = error.status();
    instance.attr("status_code") = status.code ? py::cast(ista::toString(*status.code)) : py::none();
    instance.attr("capability_reason") = status.capability_reason ? py::cast(ista::toString(*status.capability_reason)) : py::none();
    instance.attr("state") = ista::toString(status.state);
    py::dict request;
    request["backend"] = ista::toString(error.request().backend);
    request["update_strategy"] = ista::toString(error.request().update_strategy);
    request["fallback_policy"] = ista::toString(error.request().fallback_policy);
    instance.attr("request") = std::move(request);
    PyErr_SetObject(sta_session_exception_type, instance.ptr());
  }
}

inline void registerStaSessionException(py::module& m)
{
  const std::string qualified_name = py::str(m.attr("__name__")).cast<std::string>() + ".StaSessionError";
  PyObject* exception_type = PyErr_NewException(qualified_name.c_str(), PyExc_RuntimeError, nullptr);
  if (exception_type == nullptr)
    throw py::error_already_set();
  m.attr("StaSessionError") = py::reinterpret_borrow<py::object>(exception_type);
  sta_session_exception_type = exception_type;
  py::register_exception_translator(translateStaSessionException);
}

void register_ista(py::module& m)
{
  registerStaSessionException(m);
  m.def("init_sta", initSTA, py::arg("config") = "", py::arg("config_dict") = std::map<std::string, std::string>{});
  m.def("run_sta", runSTA);
  m.def("extract_lib", extractLib);
  m.def("destroy_sta", destroySTA);
  m.def("resolve_sta_execution", resolveStaExecution, py::arg("request"));
  py::class_<ista::IncrementalStaSession>(m, "IncrementalStaSession")
      .def("initialize", initializeIncrementalStaSession, py::arg("config") = "",
           py::arg("config_dict") = std::map<std::string, std::string>{})
      .def("run_full_baseline", runIncrementalStaBaseline)
      .def("close", closeIncrementalStaSession)
      .def_property_readonly("state", [](const ista::IncrementalStaSession& session) { return ista::toString(session.state()); });
  m.def("create_incremental_sta_session", createIncrementalStaSession, py::arg("request"));
}

}  // namespace python_interface
