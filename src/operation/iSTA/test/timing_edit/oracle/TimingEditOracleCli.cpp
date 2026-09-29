// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "TimingEditOracleCli.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

namespace ista::timing_edit::oracle {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(std::string message)
{
  throw std::runtime_error("timing-edit oracle CLI: " + std::move(message));
}

std::size_t parsePrefix(std::string_view text)
{
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
    fail("--prefix must be a positive canonical integer");
  }
  return value;
}

const std::string& require(const std::multimap<std::string, std::string>& values,
                           std::string_view flag)
{
  const auto found = values.find(std::string(flag));
  if (found == values.end() || found->second.empty() || values.count(std::string(flag)) != 1) {
    fail("missing command-line flag: " + std::string(flag));
  }
  return found->second;
}

fs::path requireAbsolute(const std::string& text, std::string_view flag)
{
  const fs::path path = text;
  if (!path.is_absolute()) {
    fail(std::string(flag) + " must be absolute");
  }
  return path;
}

FreshBaselineArtifacts artifacts(const std::multimap<std::string, std::string>& values)
{
  FreshBaselineArtifacts result;
  result.tech_lef = requireAbsolute(require(values, "--tech-lef"), "--tech-lef");
  const auto append_paths = [&](const std::string& flag, auto& paths) {
    const auto [first, last] = values.equal_range(flag);
    if (first == last)
      fail("missing command-line flag: " + flag);
    for (auto it = first; it != last; ++it)
      paths.push_back(requireAbsolute(it->second, flag));
  };
  append_paths("--cell-lef", result.cell_lefs);
  append_paths("--liberty", result.liberties);
  result.analysis_context = {"slow", "25", "__no_rc_corner__"};
  if (values.contains("--liberty-corner") || values.contains("--temperature") || values.contains("--rc-corner"))
    result.analysis_context = {require(values, "--liberty-corner"), require(values, "--temperature"), require(values, "--rc-corner")};
  result.verilog = requireAbsolute(require(values, "--verilog"), "--verilog");
  result.sdc = requireAbsolute(require(values, "--sdc"), "--sdc");
  result.top = require(values, "--top");
  result.run_directory = requireAbsolute(require(values, "--run-dir"), "--run-dir");
  return result;
}

fs::path canonicalExecutable(const char* argv0)
{
  std::error_code error;
  fs::path result = fs::canonical(fs::path(argv0), error);
  if (error || !result.is_absolute()) {
    fail("probe executable cannot be canonicalized");
  }
  return result;
}

fs::path requireRegularTrace(const fs::path& path)
{
  if (!path.is_absolute()) {
    fail("--trace must be absolute");
  }
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (error || status.type() != fs::file_type::regular) {
    fail("--trace must name a regular non-symlink file");
  }
  return path;
}

std::string readText(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail("cannot open trace file");
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) {
    fail("cannot size trace file");
  }
  input.seekg(0, std::ios::beg);
  std::string text(static_cast<std::size_t>(size), '\0');
  if (size != 0 && !input.read(text.data(), size)) {
    fail("cannot read complete trace file");
  }
  return text;
}

}  // namespace

std::string_view token(OracleExecutionMode value)
{
  switch (value) {
    case OracleExecutionMode::kCpuFull:
      return "cpu/full";
    case OracleExecutionMode::kCpuIncremental:
      return "cpu/incremental";
    case OracleExecutionMode::kGpuFull:
      return "gpu/full";
    case OracleExecutionMode::kGpuIncremental:
      return "gpu/incremental";
  }
  throw std::invalid_argument("unknown OracleExecutionMode");
}

std::string_view token(OracleModeDisposition value)
{
  switch (value) {
    case OracleModeDisposition::kImplemented:
      return "implemented";
    case OracleModeDisposition::kUnsupported:
      return "unsupported";
  }
  throw std::invalid_argument("unknown OracleModeDisposition");
}

std::string_view token(OracleModeUnavailableReason value)
{
  switch (value) {
    case OracleModeUnavailableReason::kStageNotImplemented:
      return "stage_not_implemented";
  }
  throw std::invalid_argument("unknown OracleModeUnavailableReason");
}

OracleModeStatus statusFor(OracleExecutionMode mode) noexcept
{
  if (mode == OracleExecutionMode::kCpuFull) {
    return {mode, OracleModeDisposition::kImplemented, std::nullopt};
  }
  return {mode, OracleModeDisposition::kUnsupported,
          OracleModeUnavailableReason::kStageNotImplemented};
}

std::string canonicalOracleModeStatusText(const OracleModeStatus& status)
{
  std::string result = "mode_status|" + std::string(token(status.mode)) + "|"
                       + std::string(token(status.disposition)) + "|";
  if (status.reason) {
    result += token(*status.reason);
  } else {
    result += "none";
  }
  result.push_back('\n');
  return result;
}

OracleExecutionMode parseMode(std::string_view value)
{
  if (value == "cpu/full") return OracleExecutionMode::kCpuFull;
  if (value == "cpu/incremental") return OracleExecutionMode::kCpuIncremental;
  if (value == "gpu/full") return OracleExecutionMode::kGpuFull;
  if (value == "gpu/incremental") return OracleExecutionMode::kGpuIncremental;
  fail("--mode has an unknown execution mode");
}

OracleSubject parseSubject(std::string_view value)
{
  if (value == "correct") return OracleSubject::kCorrect;
  if (value == "wrong-sink") return OracleSubject::kWrongSink;
  if (value == "derived-tamper") return OracleSubject::kDerivedTamper;
  fail("--subject must be correct, wrong-sink, or derived-tamper");
}

OracleCliMode parseOracleCli(int argc, char* argv[], bool allow_trace_creation)
{
  if (argc < 3 || (argc - 1) % 2 != 0) {
    fail("expected key/value command-line arguments");
  }
  const std::set<std::string> common{"--tech-lef", "--cell-lef", "--liberty", "--verilog",
                                     "--sdc", "--top", "--run-dir"};
  std::set<std::string> allowed = common;
  allowed.insert({"--liberty-corner", "--temperature", "--rc-corner"});
  allowed.insert("--trace");
  allowed.insert("--prefix");
  allowed.insert("--output");
  allowed.insert("--mode");
  allowed.insert("--subject");
  const std::set<std::string> creation_flags{
      "--net", "--sink", "--buffer", "--input", "--output-port", "--profile", "--seed", "--data-side"};
  if (allow_trace_creation) {
    allowed.insert(creation_flags.begin(), creation_flags.end());
    allowed.insert("--create-trace");
  }

  std::multimap<std::string, std::string> values;
  for (int index = 1; index < argc; index += 2) {
    const std::string flag = argv[index];
    if (!allowed.contains(flag)) {
      fail("unknown command-line flag: " + flag);
    }
    if (values.contains(flag) && flag != "--cell-lef" && flag != "--liberty")
      fail("duplicate command-line flag: " + flag);
    values.emplace(flag, argv[index + 1]);
  }
  for (const std::string& flag : common) {
    if (flag != "--cell-lef" && flag != "--liberty")
      static_cast<void>(require(values, flag));
  }

  const bool create = values.contains("--create-trace");
  const bool replay = values.contains("--trace");
  if (create == replay) {
    fail("select exactly one of --trace or --create-trace");
  }
  const fs::path executable = canonicalExecutable(argv[0]);
  if (create) {
    if (!allow_trace_creation || values.contains("--prefix") || values.contains("--output")
        || values.contains("--mode") || values.contains("--subject")) {
      fail("trace creation accepts neither --prefix nor --output");
    }
    OracleTraceCreationOptions result{artifacts(values),
                                     requireAbsolute(require(values, "--create-trace"), "--create-trace"), executable};
    if (values.contains("--data-side")) {
      result.data_side = require(values, "--data-side");
      if (result.data_side != "q" && result.data_side != "d" && result.data_side != "data")
        fail("--data-side must be q, d, or data");
      if (values.contains("--net") || values.contains("--sink"))
        fail("--data-side cannot be combined with an explicit net/sink");
    }
    if (values.contains("--net")) result.net = require(values, "--net");
    if (values.contains("--sink")) result.sink = require(values, "--sink");
    if (values.contains("--buffer")) result.buffer = require(values, "--buffer");
    if (values.contains("--input")) result.input_port = require(values, "--input");
    if (values.contains("--output-port")) result.output_port = require(values, "--output-port");
    if (values.contains("--profile")) result.profile = require(values, "--profile");
    if (values.contains("--seed")) {
      const auto& seed = require(values, "--seed");
      const auto parsed = std::from_chars(seed.data(), seed.data() + seed.size(), result.seed);
      if (parsed.ec != std::errc{} || parsed.ptr != seed.data() + seed.size())
        fail("--seed must be an unsigned integer");
    }
    if (!result.data_side.empty() && result.seed != 0)
      fail("--data-side real replay requires --seed 0");
    return result;
  }
  if (values.contains("--create-trace")) {
    fail("trace creation is not supported by this probe");
  }
  for (const auto& flag : creation_flags)
    if (values.contains(flag))
      fail("creation-only option used for replay: " + flag);
  OracleExecutionMode mode = OracleExecutionMode::kCpuFull;
  if (const auto found = values.find("--mode"); found != values.end()) {
    mode = parseMode(found->second);
  }
  OracleSubject subject = OracleSubject::kCorrect;
  if (const auto found = values.find("--subject"); found != values.end()) {
    subject = parseSubject(found->second);
  }
  return OracleReplayOptions{artifacts(values),
                             requireAbsolute(require(values, "--trace"), "--trace"),
                             requireAbsolute(require(values, "--output"), "--output"),
                             executable, parsePrefix(require(values, "--prefix")), mode, subject};
}

TimingEditTrace readStrictTimingEditTrace(const fs::path& path)
{
  TimingEditTraceParseResult parsed = parseTimingEditTraceJson(readText(requireRegularTrace(path)));
  if (auto* trace = std::get_if<TimingEditTrace>(&parsed)) {
    return std::move(*trace);
  }
  const auto& errors = std::get<TimingEditContractViolations>(parsed);
  if (errors.empty()) {
    fail("strict trace parser returned an empty failure");
  }
  fail("strict trace parse failed at " + errors.front().field + ": " + errors.front().message);
}

void writeOracleFile(const fs::path& path, std::string_view contents)
{
  if (!path.is_absolute()) {
    fail("output path must be absolute");
  }
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) {
    fail("output parent directory cannot be created");
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(contents.data(), static_cast<std::streamsize>(contents.size()))
      || !output.flush()) {
    fail("cannot write complete output file");
  }
}

void requireTraceSourceArtifacts(const TimingEditTrace& trace,
                                 const std::vector<FreshArtifactEvidence>& actual)
{
  std::vector<SourceArtifactIdentity> expected = trace.source_artifacts;
  std::vector<SourceArtifactIdentity> observed;
  observed.reserve(actual.size());
  for (const FreshArtifactEvidence& item : actual) {
    observed.push_back({item.role, item.logical_name, item.content_sha256});
  }
  const auto less = [](const SourceArtifactIdentity& lhs, const SourceArtifactIdentity& rhs) {
    return std::tie(lhs.artifact_role, lhs.logical_name, lhs.content_sha256)
           < std::tie(rhs.artifact_role, rhs.logical_name, rhs.content_sha256);
  };
  std::ranges::sort(expected, less);
  std::ranges::sort(observed, less);
  if (expected != observed) {
    fail("trace source artifacts differ from the loaded baseline");
  }
}

std::string_view subjectToken(OracleSubject subject)
{
  switch (subject) {
    case OracleSubject::kCorrect:
      return "correct";
    case OracleSubject::kWrongSink:
      return "wrong-sink";
    case OracleSubject::kDerivedTamper:
      return "derived-tamper";
  }
  fail("unknown oracle subject");
}

std::vector<std::string> makeReplayArguments(const FreshBaselineArtifacts& artifacts,
                                             const fs::path& trace, std::size_t prefix,
                                             const fs::path& output,
                                             const fs::path& run_directory,
                                             OracleExecutionMode mode,
                                             OracleSubject subject)
{
  std::vector<std::string> result{
          "--tech-lef", artifacts.tech_lef.string(), "--verilog", artifacts.verilog.string(),
          "--sdc", artifacts.sdc.string(), "--top", artifacts.top,
          "--run-dir", run_directory.string(), "--trace", trace.string(),
          "--prefix", std::to_string(prefix), "--output", output.string()};
  for (const auto& path : artifacts.cell_lefs)
    result.insert(result.end(), {"--cell-lef", path.string()});
  for (const auto& path : artifacts.liberties)
    result.insert(result.end(), {"--liberty", path.string()});
  result.insert(result.end(), {"--liberty-corner", artifacts.analysis_context.liberty_corner,
                               "--temperature", artifacts.analysis_context.temperature,
                               "--rc-corner", artifacts.analysis_context.rc_corner});
  result.insert(result.end(), {"--mode", std::string(token(mode)), "--subject",
                               std::string(subjectToken(subject))});
  return result;
}

}  // namespace ista::timing_edit::oracle
