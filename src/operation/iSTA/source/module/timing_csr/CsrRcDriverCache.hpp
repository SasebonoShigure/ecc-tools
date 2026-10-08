// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <cstdint>
#include <limits>

#include "CsrArnoldiDriver.hpp"
#include "CsrDmpDriver.hpp"

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {

inline constexpr std::uint64_t kCsrRcAbsent = std::numeric_limits<std::uint64_t>::max();
enum class CsrRcDriverFamily : std::uint8_t { unavailable, arnoldi, dmp };

// A row belongs to one native child identity, driver, analysis and output.
// Model interning does not identify a row: an Instance CTQ copy is distinct
// from a value-equal Liberty graph child. Each row has a checked preparation-
// time capacity derived from actual initialization/clock/data/CTQ calls.
struct CsrRcTimingRow
{
  std::uint64_t begin{0}, capacity{0}, size{0};
};

struct CsrRcTimingRecord
{
  double input_slew{0.0};
  std::uint64_t child_model{0};
  std::uint64_t miss_sequence{0};
  CsrRcDriverFamily family{CsrRcDriverFamily::unavailable};
  CsrArnoldiDriverResult arnoldi;
  CsrDmpDriverResult dmp;
};

struct CsrRcDriverLane
{
  std::uint64_t row_begin{0}, row_count{0};
  std::uint64_t miss_sequence{0};
  std::uint64_t selected_arnoldi{kCsrRcAbsent}, selected_dmp{kCsrRcAbsent};
};

// Called only by the driver owner. Rows and record ranges are checked during
// preparation. A hit is read-only, including unavailable results and the
// second CUDA winner scan.
ISTA_RC_HD inline std::uint64_t findCsrRcTimingRecord(
    const CsrRcTimingRecord* records, const CsrRcTimingRow& row, double input_slew)
{
  for (std::uint64_t i = 0; i < row.size; ++i) {
    const auto& record = records[row.begin + i];
    // Arnoldi uses a double tuple; native DMP formats 17-digit string keys,
    // where -0 and +0 differ. Finite nonzero doubles round-trip uniquely.
    if (record.input_slew == input_slew
        && (record.family == CsrRcDriverFamily::arnoldi || input_slew != 0.0
            || std::signbit(record.input_slew) == std::signbit(input_slew)))
      return row.begin + i;
  }
  return kCsrRcAbsent;
}

// Prepare the numeric result locally; publish only a completed cache miss.
// A failed numeric computation is not published or counted.
ISTA_RC_HD inline bool appendCsrRcTimingRecord(
    CsrRcTimingRecord* records, CsrRcTimingRow& row, CsrRcDriverLane& driver,
    CsrRcTimingRecord pending, std::uint64_t& index)
{
  index = kCsrRcAbsent;
  if (row.size >= row.capacity || driver.miss_sequence == kCsrRcAbsent
      || !std::isfinite(pending.input_slew) || pending.input_slew < 0.0)
    return false;
  const double gate_delay = pending.family == CsrRcDriverFamily::arnoldi
                                ? pending.arnoldi.gate_delay : pending.dmp.gate_delay;
  const double slew = pending.family == CsrRcDriverFamily::arnoldi
                          ? pending.arnoldi.driver_slew : pending.dmp.driver_slew;
  if (pending.family != CsrRcDriverFamily::unavailable
      && (!std::isfinite(gate_delay) || !std::isfinite(slew) || slew < 0.0))
    return false;
  pending.miss_sequence = driver.miss_sequence + 1;
  index = row.begin + row.size;
  records[index] = pending;
  ++row.size;
  ++driver.miss_sequence;
  return true;
}

// Once a clock/CTQ/data-slew phase has selected the driver's actual slew,
// choose the last real miss in each family. Downstream sinks read these two
// indices; no per-sink candidate scan or per-candidate fanout load-map.
ISTA_RC_HD inline void selectCsrRcDriverRecords(
    const CsrRcTimingRecord* records, const CsrRcTimingRow* rows,
    CsrRcDriverLane& driver, double slew)
{
  driver.selected_arnoldi = kCsrRcAbsent;
  driver.selected_dmp = kCsrRcAbsent;
  for (std::uint64_t i = 0; i < driver.row_count; ++i) {
    const auto& row = rows[driver.row_begin + i];
    for (std::uint64_t j = 0; j < row.size; ++j) {
      const auto index = row.begin + j;
      const auto& record = records[index];
      if (record.family == CsrRcDriverFamily::arnoldi && record.arnoldi.driver_slew == slew) {
        if (driver.selected_arnoldi == kCsrRcAbsent
            || record.miss_sequence > records[driver.selected_arnoldi].miss_sequence)
          driver.selected_arnoldi = index;
      } else if (record.family == CsrRcDriverFamily::dmp && record.dmp.driver_slew == slew
                 && (slew != 0.0 || std::signbit(record.dmp.driver_slew) == std::signbit(slew))) {
        if (driver.selected_dmp == kCsrRcAbsent
            || record.miss_sequence > records[driver.selected_dmp].miss_sequence)
          driver.selected_dmp = index;
      }
    }
  }
}

}  // namespace ista::csr
#undef ISTA_RC_HD
