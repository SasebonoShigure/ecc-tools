// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <variant>

#include "TimingEditTypes.hpp"
#include "TimingSemanticKey.hpp"
#include "json.hpp"

namespace ista::timing_edit::test::semantic_json {

using Json = nlohmann::json;

template <typename... Visitors>
struct Overloaded : Visitors...
{
  using Visitors::operator()...;
};
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

template <typename Range, typename Convert>
Json arrayOf(const Range& values, Convert convert)
{
  Json result = Json::array();
  for (const auto& value : values) {
    result.push_back(convert(value));
  }
  return result;
}

inline Json creationToken(const timing_edit::CreationToken& value)
{
  return {{"schema_version", value.schema_version}, {"case_id", value.case_id},
          {"profile_id", value.profile_id},         {"seed", value.seed},
          {"batch_ordinal", value.batch_ordinal},   {"accepted_ordinal", value.accepted_ordinal}};
}

inline Json nodeKey(const csr::NodeKey& value)
{
  return {{"kind", csr::token(value.kind)}, {"full_name", value.full_name}, {"instance_name", value.instance_name}, {"pin_name", value.pin_name}};
}

inline Json tableSignature(const csr::TimingTableSignature& value)
{
  return {{"present", value.present}, {"variable1", csr::token(value.variable1)}, {"variable2", csr::token(value.variable2)}};
}

inline Json librarySignature(const csr::LibraryArcSignature& value)
{
  return {{"library_name", value.library_name},
          {"arc_index", value.arc_index},
          {"sense", csr::token(value.sense)},
          {"trigger_transition", csr::token(value.trigger_transition)},
          {"check_transition", csr::token(value.check_transition)},
          {"sdf_condition", value.sdf_condition},
          {"delay_tables", arrayOf(value.delay_tables, tableSignature)},
          {"slew_tables", arrayOf(value.slew_tables, tableSignature)}};
}

inline Json arcKey(const csr::ArcKey& value)
{
  return {{"source", nodeKey(value.base.source)},
          {"sink", nodeKey(value.base.sink)},
          {"kind", csr::token(value.base.kind)},
          {"owner_name", value.base.owner_name},
          {"library_source_port", value.base.library_source_port},
          {"library_sink_port", value.base.library_sink_port},
          {"library_arcs", arrayOf(value.base.library_arcs, librarySignature)},
          {"parallel_ordinal", value.parallel_ordinal}};
}

inline Json stableNode(const timing_edit::StableNodeRef& value)
{
  return std::visit(
      Overloaded{[](const timing_edit::ExistingNodeRef& item) { return Json{{"kind", "existing"}, {"baseline_key", nodeKey(item.baseline_key)}}; },
                 [](const timing_edit::CreatedNodeRef& item) {
                   return Json{{"kind", "created"}, {"creation_token", creationToken(item.creation_token)}, {"role", timing_edit::token(item.role)}};
                 }},
      value);
}

inline Json stableNet(const timing_edit::StableNetRef& value)
{
  return std::visit(Overloaded{[](const timing_edit::ExistingNetRef& item) { return Json{{"kind", "existing"}, {"baseline_name", item.baseline_name}}; },
                               [](const timing_edit::CreatedNetRef& item) {
                                 return Json{
                                     {"kind", "created"}, {"creation_token", creationToken(item.creation_token)}, {"role", timing_edit::token(item.role)}};
                               }},
                    value);
}

inline Json stableArc(const timing_edit::StableArcRef& value)
{
  return std::visit(Overloaded{[](const timing_edit::ExistingArcRef& item) { return Json{{"kind", "existing"}, {"baseline_key", arcKey(item.baseline_key)}}; },
                               [](const timing_edit::CreatedCellArcRef& item) {
                                 return Json{{"kind", "created_cell"},
                                             {"creation_token", creationToken(item.creation_token)},
                                             {"library_source_port", item.library_source_port},
                                             {"library_sink_port", item.library_sink_port},
                                             {"ordered_library_arcs", arrayOf(item.ordered_library_arcs, librarySignature)},
                                             {"stable_equivalence_ordinal", item.stable_equivalence_ordinal}};
                               },
                               [](const timing_edit::CreatedOldNetArcRef& item) {
                                 return Json{{"kind", "created_old_net"},
                                             {"net", stableNet(item.net)},
                                             {"driver", stableNode(item.driver)},
                                             {"sink", stableNode(item.sink)},
                                             {"stable_ordinal", item.stable_ordinal}};
                               },
                               [](const timing_edit::CreatedNewNetArcRef& item) {
                                 return Json{{"kind", "created_new_net"},
                                             {"creation_token", creationToken(item.creation_token)},
                                             {"driver", stableNode(item.driver)},
                                             {"sink", stableNode(item.sink)},
                                             {"stable_ordinal", item.stable_ordinal}};
                               }},
                    value);
}

}  // namespace ista::timing_edit::test::semantic_json
