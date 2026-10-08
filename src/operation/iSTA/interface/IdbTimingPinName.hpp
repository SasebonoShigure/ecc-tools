// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <stdexcept>
#include <string>
#include "IdbInstance.h"

namespace ista {
// The canonical IDB -> native STA identity, shared by baseline and physical edits.
inline std::string idbTimingPinName(idb::IdbPin& pin)
{
  if (pin.is_io_pin()) return pin.get_pin_name();
  if (!pin.get_instance()) throw std::invalid_argument("timing pin has no instance");
  return pin.get_instance()->get_name() + ":" + pin.get_pin_name();
}
}  // namespace ista
