// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "IndependentFreshBaselineLoader.hpp"
#include "TimingEditTrace.hpp"

namespace ista::timing_edit::oracle {

enum class OracleExecutionMode : std::uint8_t
{
  kCpuFull,
  kCpuIncremental,
  kGpuFull,
  kGpuIncremental
};

enum class OracleModeDisposition : std::uint8_t
{
  kImplemented,
  kUnsupported
};

enum class OracleModeUnavailableReason : std::uint8_t
{
  kStageNotImplemented
};

struct OracleModeStatus
{
  OracleExecutionMode mode{OracleExecutionMode::kCpuFull};
  OracleModeDisposition disposition{OracleModeDisposition::kImplemented};
  std::optional<OracleModeUnavailableReason> reason;

  friend bool operator==(const OracleModeStatus&, const OracleModeStatus&) = default;
};

enum class OracleSubject : std::uint8_t
{
  kCorrect,
  kWrongSink,
  kDerivedTamper
};

[[nodiscard]] std::string_view token(OracleExecutionMode value);
[[nodiscard]] std::string_view token(OracleModeDisposition value);
[[nodiscard]] std::string_view token(OracleModeUnavailableReason value);
[[nodiscard]] OracleModeStatus statusFor(OracleExecutionMode mode) noexcept;
[[nodiscard]] std::string canonicalOracleModeStatusText(const OracleModeStatus& status);

struct OracleReplayOptions
{
  FreshBaselineArtifacts artifacts;
  std::filesystem::path trace;
  std::filesystem::path output;
  std::filesystem::path executable;
  std::size_t prefix{0};
  OracleExecutionMode mode{OracleExecutionMode::kCpuFull};
  OracleSubject subject{OracleSubject::kCorrect};
};

struct OracleTraceCreationOptions
{
  FreshBaselineArtifacts artifacts;
  std::filesystem::path trace_output;
  std::filesystem::path executable;
  std::string net{"n0"};
  std::string sink{"u1:A"};
  std::string buffer{"BUF_X1"};
  std::string input_port{"A"};
  std::string output_port{"Y"};
  std::string profile{"stage3_real_cpu_full"};
  std::uint64_t seed{20260831};
};

using OracleCliMode = std::variant<OracleReplayOptions, OracleTraceCreationOptions>;

// Parses a closed set of key/value flags. Replay always requires a durable
// --trace file and an explicit --prefix; the creation mode is available only
// to the independent fixture builder.
[[nodiscard]] OracleCliMode parseOracleCli(int argc, char* argv[], bool allow_trace_creation);

[[nodiscard]] TimingEditTrace readStrictTimingEditTrace(const std::filesystem::path& path);

void writeOracleFile(const std::filesystem::path& path, std::string_view contents);

void requireTraceSourceArtifacts(const TimingEditTrace& trace,
                                 const std::vector<FreshArtifactEvidence>& actual);

[[nodiscard]] std::vector<std::string> makeReplayArguments(
    const FreshBaselineArtifacts& artifacts, const std::filesystem::path& trace,
    std::size_t prefix, const std::filesystem::path& output,
    const std::filesystem::path& run_directory,
    OracleExecutionMode mode = OracleExecutionMode::kCpuFull,
    OracleSubject subject = OracleSubject::kCorrect);

}  // namespace ista::timing_edit::oracle
