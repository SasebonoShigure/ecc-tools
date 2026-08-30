// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <optional>
#include <stdexcept>

#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputRawProjectionInternal.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "TimingCell.hpp"
#include "TimingCheckArc.hpp"
#include "TimingClock.hpp"
#include "TimingConstraint.hpp"
#include "TimingLibrary.hpp"
#include "TimingPortConstraint.hpp"

namespace ista::csr::test {
namespace {

RawTimingCheckKind checkKindToken(ista::TimingCheckType kind)
{
  switch (kind) {
    case ista::TimingCheckType::kNone:
      return RawTimingCheckKind::kNone;
    case ista::TimingCheckType::kSetup:
      return RawTimingCheckKind::kSetup;
    case ista::TimingCheckType::kHold:
      return RawTimingCheckKind::kHold;
    case ista::TimingCheckType::kRecovery:
      return RawTimingCheckKind::kRecovery;
    case ista::TimingCheckType::kRemoval:
      return RawTimingCheckKind::kRemoval;
    case ista::TimingCheckType::kWidth:
      return RawTimingCheckKind::kWidth;
    case ista::TimingCheckType::kPeriod:
      return RawTimingCheckKind::kPeriod;
  }
  throw std::runtime_error("raw constraint projector: unknown check kind");
}

std::optional<double> present(bool condition, double value)
{
  return condition ? std::optional<double>{value} : std::nullopt;
}

void projectChecks(const ista::Database& database, CsrReferenceInputRawProjection& projection)
{
  for (const auto& [instance_name, instance] : database.get_instance_map()) {
    const auto cell = database.get_timing_library().get_cell_map().find(instance.get_cell_name());
    const std::vector<ista::TimingCheckArc>* library_checks
        = cell == database.get_timing_library().get_cell_map().end() ? nullptr : &cell->second.get_check_arc_list();
    for (std::size_t ordinal = 0; ordinal < instance.get_check_arc_list().size(); ++ordinal) {
      const auto& native = instance.get_check_arc_list().at(ordinal);
      const auto* library = library_checks != nullptr && ordinal < library_checks->size() ? &library_checks->at(ordinal) : nullptr;
      RawTimingCheck check;
      check.instance_name = instance_name;
      check.cell_name = instance.get_cell_name();
      check.ordinal = static_cast<std::uint64_t>(ordinal);
      check.kind = checkKindToken(native.get_check_type());
      check.raw_clock_name = library == nullptr ? native.get_clock_port() : library->get_clock_port();
      check.raw_data_name = library == nullptr ? native.get_data_port() : library->get_data_port();
      check.full_clock_name = native.get_clock_port();
      check.full_data_name = native.get_data_port();
      projection.timing_checks.push_back(std::move(check));
    }
  }
}

}  // namespace

void projectCsrReferenceInputRawConstraints(const ista::Database& database, CsrReferenceInputRawProjection& projection)
{
  const auto& constraints = database.get_timing_constraint();
  projection.clock_constraints.reserve(constraints.get_clock_map().size());
  for (const auto& [map_key, native] : constraints.get_clock_map())
    projection.clock_constraints.push_back({map_key, native.get_clock_name(), native.get_source_list(), native.get_period(), native.get_rise_edge(),
                                            native.get_fall_edge(), native.get_is_propagated()});

  projection.port_constraints.reserve(constraints.get_port_constraint_map().size());
  for (const auto& [map_key, native] : constraints.get_port_constraint_map()) {
    RawPortConstraint port;
    port.map_key = map_key;
    port.embedded_port_name = native.get_port_name();
    port.clock_name = native.get_clock_name();
    port.input_delay_max = present(native.get_has_input_delay_max(), native.get_input_delay_max());
    port.input_delay_min = present(native.get_has_input_delay_min(), native.get_input_delay_min());
    port.input_transition = present(native.get_has_input_transition(), native.get_input_transition());
    port.output_delay_max = present(native.get_has_output_delay_max(), native.get_output_delay_max());
    port.output_delay_min = present(native.get_has_output_delay_min(), native.get_output_delay_min());
    port.output_load = present(native.get_has_load(), native.get_load());
    projection.port_constraints.push_back(std::move(port));
  }

  projection.case_constraints.reserve(constraints.get_case_analysis_map().size());
  for (const auto& [map_key, value] : constraints.get_case_analysis_map())
    projection.case_constraints.push_back({map_key, map_key, value});
  projectChecks(database, projection);
}

namespace raw_wire {

void appendClock(Writer& writer, const RawClockConstraint& clock)
{
  writer.string(clock.map_key);
  writer.string(clock.embedded_name);
  writer.vector(clock.ordered_sources, [](Writer& local, const std::string& value) { local.string(value); });
  writer.finite(clock.period);
  writer.finite(clock.rise_edge);
  writer.finite(clock.fall_edge);
  writer.boolean(clock.propagated);
}

void appendPort(Writer& writer, const RawPortConstraint& port)
{
  writer.string(port.map_key);
  writer.string(port.embedded_port_name);
  writer.string(port.clock_name);
  optionalDouble(writer, port.input_delay_max);
  optionalDouble(writer, port.input_delay_min);
  optionalDouble(writer, port.input_transition);
  optionalDouble(writer, port.output_delay_max);
  optionalDouble(writer, port.output_delay_min);
  optionalDouble(writer, port.output_load);
}

void appendCase(Writer& writer, const RawCaseConstraint& constraint)
{
  writer.string(constraint.map_key);
  writer.string(constraint.raw_name);
  writer.boolean(constraint.value);
}

void appendCheck(Writer& writer, const RawTimingCheck& check)
{
  writer.string(check.instance_name);
  writer.string(check.cell_name);
  writer.u64(check.ordinal);
  writer.enumeration(check.kind);
  writer.string(check.raw_clock_name);
  writer.string(check.full_clock_name);
  writer.string(check.raw_data_name);
  writer.string(check.full_data_name);
}

}  // namespace raw_wire
}  // namespace ista::csr::test
