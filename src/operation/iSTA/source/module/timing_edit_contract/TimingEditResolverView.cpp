// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingEditResolver.hpp"
#include <stdexcept>

namespace ista::timing_edit {

namespace {
[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("timing edit resolver: " + std::move(message));
}

template <class Map, class Key>
const typename Map::value_type* findEntry(const Map& values, const Key& key)
{
  const auto found = values.find(key);
  return found == values.end() ? nullptr : &*found;
}
template <class Map, class Key>
const typename Map::mapped_type* findValue(const Map& values, const Key& key)
{
  const auto* entry = findEntry(values, key);
  return entry ? &entry->second : nullptr;
}
const NativeSlotBinding& requireLive(const NativeSlotBinding* binding, const char* kind)
{
  if (!binding || binding->tombstone)
    invalid(std::string(kind) + " reference is missing or tombstoned");
  return *binding;
}
}  // namespace

const NodeBindingEntry* TimingEditResolverView::findNodeEntry(const StableNodeRef& ref) const
{
  if (_delta)
    if (const auto* value = findEntry(_delta->_changes._nodes, ref)) return value;
  return findEntry(_base._nodes, ref);
}
const NativeSlotBinding* TimingEditResolverView::findNodeBinding(const StableNodeRef& ref) const
{
  const auto* entry = findNodeEntry(ref);
  return entry ? &entry->second : nullptr;
}
const NodeBindingEntry* TimingEditResolverView::findNodeEntryForKey(const csr::NodeKey& key) const
{
  if (const auto* baseline = findNodeEntry(ExistingNodeRef{key})) return baseline;
  // shortcut: created identities scan edit history; index only if this lookup dominates.
  const auto created = [&](const auto& values) -> const NodeBindingEntry* {
    for (const auto& [token, names] : values) {
      if (names.input_pin_name == key.full_name) return findNodeEntry(CreatedNodeRef{token, CreatedNodeRole::kInput});
      if (names.output_pin_name == key.full_name) return findNodeEntry(CreatedNodeRef{token, CreatedNodeRole::kOutput});
    }
    return nullptr;
  };
  if (_delta)
    if (const auto* entry = created(_delta->_changes._materializations)) return entry;
  return created(_base._materializations);
}
const NetBindingEntry* TimingEditResolverView::findNetEntry(const StableNetRef& ref) const
{
  if (_delta)
    if (const auto* value = findEntry(_delta->_changes._nets, ref)) return value;
  return findEntry(_base._nets, ref);
}
const NativeSlotBinding* TimingEditResolverView::findNetBinding(const StableNetRef& ref) const
{
  const auto* entry = findNetEntry(ref);
  return entry ? &entry->second : nullptr;
}
const NetBindingEntry* TimingEditResolverView::findNetEntryForName(const std::string& name) const
{
  if (const auto* baseline = findNetEntry(ExistingNetRef{name})) return baseline;
  const auto created = [&](const auto& values) -> const NetBindingEntry* {
    for (const auto& [token, names] : values)
      if (names.output_net_name == name) return findNetEntry(CreatedNetRef{token, CreatedNetRole::kOutputNet});
    return nullptr;
  };
  if (_delta)
    if (const auto* entry = created(_delta->_changes._materializations)) return entry;
  return created(_base._materializations);
}
const NativeSlotBinding* TimingEditResolverView::findArcBinding(const StableArcRef& ref) const
{
  if (_delta)
    if (const auto* value = findValue(_delta->_changes._arcs, ref))
      return value;
  return findValue(_base._arcs, ref);
}
const std::string& TimingEditResolverView::resolveNode(const StableNodeRef& ref) const
{
  return requireLive(findNodeBinding(ref), "node").native_name;
}
const std::string& TimingEditResolverView::resolveNet(const StableNetRef& ref) const
{
  return requireLive(findNetBinding(ref), "net").native_name;
}
AppendOnlySlot TimingEditResolverView::resolveArcSlot(const StableArcRef& ref) const
{
  return requireLive(findArcBinding(ref), "arc").slot;
}
const TokenMaterialization* TimingEditResolverView::findMaterialization(const CreationToken& token) const
{
  if (_delta)
    if (const auto* value = findValue(_delta->_changes._materializations, token))
      return value;
  return findValue(_base._materializations, token);
}
const TokenMaterialization& TimingEditResolverView::materialization(const CreationToken& token) const
{
  const auto* value = findMaterialization(token);
  if (!value)
    invalid("creation token has no native materialization");
  return *value;
}

}  // namespace ista::timing_edit
