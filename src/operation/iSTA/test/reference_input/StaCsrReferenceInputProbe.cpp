// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <any>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ClockPropagator.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferencePoisonSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "IncrementalStaSession.hpp"
#include "Logger.hpp"
#include "STAInterface.hpp"
#include "TimingClock.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "idm.h"
#include "json.hpp"

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace ista::csr;
using namespace ista::csr::test;

struct Options
{
  fs::path tech_lef;
  fs::path cell_lef;
  fs::path liberty;
  fs::path verilog;
  fs::path sdc;
  std::string top;
  std::string case_name;
  std::string expected_clock;
  StaAnalysisContext context;
  std::uint64_t repeat_index{0};
  std::uint64_t session_token{0};
  std::string run_nonce;
  fs::path run_dir;
  fs::path result_temp;
};

struct Observation
{
  std::vector<std::uint8_t> pre_candidate;
  std::vector<std::uint8_t> post_candidate;
  std::vector<std::uint8_t> pre_raw;
  std::vector<std::uint8_t> post_raw;
  std::vector<ReferencePoisonCanary> canaries;
};

void require(bool condition, std::string_view message)
{
  if (!condition)
    throw std::runtime_error(std::string(message));
}
inline constexpr std::string_view kDifferentClockMapKeySelfTest = "--self-test-different-clock-map-key";
inline constexpr int kClockOverlapExitCode = 86;
bool g_clock_overlap_exit_armed = false;

class ScopedOutputSilence
{
 public:
  ScopedOutputSilence()
  {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    _saved_stdout = ::dup(STDOUT_FILENO);
    _saved_stderr = ::dup(STDERR_FILENO);
    const int sink = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (_saved_stdout < 0 || _saved_stderr < 0 || sink < 0)
      fail("failed to prepare probe output isolation", sink);
    if (::dup2(sink, STDOUT_FILENO) < 0 || ::dup2(sink, STDERR_FILENO) < 0)
      fail("failed to install probe output isolation", sink);
    static_cast<void>(::close(sink));
  }

  ~ScopedOutputSilence()
  {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    restore();
  }

  ScopedOutputSilence(const ScopedOutputSilence&) = delete;
  ScopedOutputSilence& operator=(const ScopedOutputSilence&) = delete;

 private:
  [[noreturn]] void fail(std::string_view message, int sink)
  {
    const int saved_errno = errno;
    if (sink >= 0)
      static_cast<void>(::close(sink));
    restore();
    throw std::runtime_error(std::string(message) + ": " + std::strerror(saved_errno));
  }

  void restore() noexcept
  {
    if (_saved_stdout >= 0) {
      static_cast<void>(::dup2(_saved_stdout, STDOUT_FILENO));
      static_cast<void>(::close(_saved_stdout));
      _saved_stdout = -1;
    }
    if (_saved_stderr >= 0) {
      static_cast<void>(::dup2(_saved_stderr, STDERR_FILENO));
      static_cast<void>(::close(_saved_stderr));
      _saved_stderr = -1;
    }
  }

  int _saved_stdout{-1};
  int _saved_stderr{-1};
};

void forceClockOverlapFailureExit()
{
  if (!g_clock_overlap_exit_armed)
    return;
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  std::_Exit(kClockOverlapExitCode);
}

int runDifferentClockMapKeySelfTest()
{
  if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    throw std::runtime_error("failed to route clock-overlap fatal output to stderr");

  ista::Logger::initInst();
  ista::DataManager::initInst();
  ista::ClockPropagator::initInst();
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  ista::TimingClock overlap;
  overlap.set_clock_name("overlap_embedded_clock");
  overlap.set_source_list({"root"});
  overlap.set_period(4.0);
  overlap.set_fall_edge(2.0);
  require(fixture.database().get_timing_constraint().get_clock_map().emplace("z_overlap_key", std::move(overlap)).second,
          "failed to install different-map-key overlap clock");
  STADM.getDatabase() = fixture.database();
  if (std::atexit(forceClockOverlapFailureExit) != 0)
    throw std::runtime_error("failed to register nonzero clock-overlap fatal exit");
  g_clock_overlap_exit_armed = true;
  STACP.propagate();

  g_clock_overlap_exit_armed = false;
  std::cerr << "ClockPropagator different-map-key overlap unexpectedly returned\n";
  std::cerr.flush();
  std::_Exit(87);
}

bool isLowerHex(std::string_view value, std::size_t size)
{
  if (value.size() != size)
    return false;
  for (char character : value)
    if ((character < '0' || character > '9') && (character < 'a' || character > 'f'))
      return false;
  return true;
}

std::uint64_t parseU64(const std::string& value, std::string_view label)
{
  if (value.empty())
    throw std::runtime_error(std::string(label) + " is empty");
  std::uint64_t result = 0;
  for (char character : value) {
    if (character < '0' || character > '9')
      throw std::runtime_error(std::string(label) + " must contain decimal digits only");
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      throw std::runtime_error(std::string(label) + " overflows U64");
    result = result * 10 + digit;
  }
  return result;
}

Options parseOptions(int argc, char* argv[])
{
  if (argc < 2 || argc % 2 == 0)
    throw std::runtime_error("arguments must be exact --option value pairs");
  std::map<std::string, std::string> values;
  for (int index = 1; index < argc; index += 2) {
    const std::string name = argv[index];
    if (!name.starts_with("--") || index + 1 >= argc)
      throw std::runtime_error("arguments must be exact --option value pairs");
    if (!values.emplace(name, argv[index + 1]).second)
      throw std::runtime_error("duplicate option: " + name);
  }
  const std::vector<std::string> required = {
      "--tech-lef",      "--cell-lef",       "--liberty",        "--verilog",     "--sdc",       "--top",
      "--case-name",     "--expected-clock", "--liberty-corner", "--temperature", "--rc-corner", "--repeat-index",
      "--session-token", "--run-nonce",      "--run-dir",        "--result-temp",
  };
  for (const auto& name : required)
    if (!values.contains(name) || values.at(name).empty())
      throw std::runtime_error("missing required option: " + name);
  if (values.size() != required.size())
    throw std::runtime_error("unknown command-line option");

  Options options;
  options.tech_lef = values.at("--tech-lef");
  options.cell_lef = values.at("--cell-lef");
  options.liberty = values.at("--liberty");
  options.verilog = values.at("--verilog");
  options.sdc = values.at("--sdc");
  options.top = values.at("--top");
  options.case_name = values.at("--case-name");
  options.expected_clock = values.at("--expected-clock");
  options.context = {values.at("--liberty-corner"), values.at("--temperature"), values.at("--rc-corner")};
  options.repeat_index = parseU64(values.at("--repeat-index"), "repeat index");
  options.session_token = parseU64(values.at("--session-token"), "session token");
  options.run_nonce = values.at("--run-nonce");
  options.run_dir = values.at("--run-dir");
  options.result_temp = values.at("--result-temp");
  return options;
}

fs::path canonicalRegularFile(const fs::path& path, std::string_view label)
{
  if (!path.is_absolute() || !fs::is_regular_file(path))
    throw std::runtime_error(std::string(label) + " must be an absolute regular file");
  return fs::canonical(path);
}

void requireAbsent(const fs::path& path, std::string_view label)
{
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (status.type() != fs::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
    throw std::runtime_error(std::string(label) + " already exists or cannot be inspected");
}

void validateOptions(Options& options)
{
  options.tech_lef = canonicalRegularFile(options.tech_lef, "technology LEF");
  options.cell_lef = canonicalRegularFile(options.cell_lef, "cell LEF");
  options.liberty = canonicalRegularFile(options.liberty, "Liberty");
  options.verilog = canonicalRegularFile(options.verilog, "Verilog");
  options.sdc = canonicalRegularFile(options.sdc, "SDC");
  if (options.case_name != "chain" && options.case_name != "diamond")
    throw std::runtime_error("case name must be chain or diamond");
  if (options.top.empty() || options.expected_clock.empty())
    throw std::runtime_error("top and expected clock must be non-empty");
  if (options.context.liberty_corner.empty() || options.context.temperature.empty() || options.context.rc_corner.empty())
    throw std::runtime_error("analysis context fields must be non-empty");
  if (!isLowerHex(options.run_nonce, 32))
    throw std::runtime_error("run nonce must be 32 lowercase hex characters");
  if (options.session_token != 1)
    throw std::runtime_error("session token must be exactly one");
  if (!options.run_dir.is_absolute() || !fs::is_directory(options.run_dir) || fs::is_symlink(options.run_dir))
    throw std::runtime_error("run directory must be an existing real absolute directory");
  options.run_dir = fs::canonical(options.run_dir);
  if (!options.result_temp.is_absolute())
    throw std::runtime_error("result temporary path must be absolute");
  const fs::path normalized = options.result_temp.lexically_normal();
  if (normalized.parent_path() != options.run_dir || normalized.filename() != ".reference-input-result-" + options.run_nonce + ".tmp")
    throw std::runtime_error("result temporary path is not the nonce-bound run-directory child");
  options.result_temp = normalized;
  requireAbsent(options.result_temp, "result temporary path");
  requireAbsent(options.run_dir / "reference-input-result.json", "controller-owned final result");
}

void loadDesign(const Options& options)
{
  if (!dmInst->readLef(std::vector<std::string>{options.tech_lef.string()}, true))
    throw std::runtime_error("failed to read technology LEF");
  if (!dmInst->readLef(std::vector<std::string>{options.cell_lef.string()}))
    throw std::runtime_error("failed to read cell LEF");
  dmInst->get_config().set_sdc_path(options.sdc.string());
  if (!dmInst->readVerilog(options.verilog.string(), options.top))
    throw std::runtime_error("failed to read Verilog");
  if (!dmInst->readLib(std::vector<std::string>{options.liberty.string()}))
    throw std::runtime_error("failed to read Liberty");
}

void requireFullStaState(const Options& options, ista::Database& database)
{
  require(database.get_timing_constraint().get_clock_map().contains(options.expected_clock), "expected clock was not created");
  require(!database.get_pin_map().empty() && !database.get_arc_list().empty(), "full STA produced an empty native graph");
  require(!database.get_end_point_list().empty() && !database.get_timing_path_group_list().empty(), "full STA produced no endpoints or timing path groups");
}

CsrReferenceInput requireInput(CsrReferenceInputExportResult result, const Options& options, std::string_view label)
{
  if (!std::holds_alternative<CsrReferenceInput>(result))
    throw std::runtime_error(std::string(label) + " returned a typed failure");
  CsrReferenceInput input = std::get<CsrReferenceInput>(std::move(result));
  if (!validateReferenceInputForSession(input, options.session_token).empty())
    throw std::runtime_error(std::string(label) + " is not strict-valid");
  return input;
}

Observation observeAndPoison(ista::Database& database, const Options& options)
{
  const CsrReferenceInput pristine
      = requireInput(exportCsrReferenceInputCandidate(database, options.context, options.session_token), options, "pristine reference-input export");
  const CsrReferenceInputRawProjection raw_before = projectCsrReferenceInputRaw(database);

  Observation result;
  result.pre_candidate = canonicalIndependentReferenceInputBytes(pristine);
  result.pre_raw = canonicalIndependentReferenceInputRawBytes(raw_before);
  const ReferencePoisonBinding binding{options.repeat_index, options.run_nonce, options.session_token};
  result.canaries = poisonReferenceDerivedState(database, raw_before, binding);

  const CsrReferenceInput post
      = requireInput(exportCsrReferenceInputCandidate(database, options.context, options.session_token), options, "post-poison reference-input export");
  const CsrReferenceInputRawProjection raw_after = projectCsrReferenceInputRaw(database);
  result.post_candidate = canonicalIndependentReferenceInputBytes(post);
  result.post_raw = canonicalIndependentReferenceInputRawBytes(raw_after);
  require(result.pre_candidate == result.post_candidate && result.pre_raw == result.post_raw, "derived poison changed detached candidate or raw projection");
  require(raw_before == raw_after && pristine.source_identity == post.source_identity, "derived poison changed raw state or source identity");
  require(result.canaries.size() == kReferencePoisonFamilyTokens.size(), "poison family count differs");
  for (std::size_t index = 0; index < result.canaries.size(); ++index)
    require(result.canaries[index].family == kReferencePoisonFamilyTokens[index] && !result.canaries[index].pristine_typed_bytes.empty()
                && !result.canaries[index].before_typed_bytes.empty() && !result.canaries[index].after_typed_bytes.empty()
                && result.canaries[index].before_typed_bytes != result.canaries[index].after_typed_bytes,
            "poison family token/order/typed bytes differ");
  return result;
}

std::string toHex(std::span<const std::uint8_t> bytes)
{
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

Json makeResult(const Options& options, const Observation& observation)
{
  Json canaries = Json::array();
  for (const auto& canary : observation.canaries)
    canaries.push_back({{"family", canary.family},
                        {"pristine_typed_bytes_hex", toHex(canary.pristine_typed_bytes)},
                        {"before_typed_bytes_hex", toHex(canary.before_typed_bytes)},
                        {"after_typed_bytes_hex", toHex(canary.after_typed_bytes)}});
  return {
      {"schema", "ista-csr-reference-input-probe/1"},
      {"name", options.case_name},
      {"top", options.top},
      {"expected_clock", options.expected_clock},
      {"context", {{"liberty_corner", options.context.liberty_corner}, {"temperature", options.context.temperature}, {"rc_corner", options.context.rc_corner}}},
      {"repeat_index", options.repeat_index},
      {"run_nonce", options.run_nonce},
      {"session_token", options.session_token},
      {"pre_candidate_wire_hex", toHex(observation.pre_candidate)},
      {"post_candidate_wire_hex", toHex(observation.post_candidate)},
      {"pre_raw_projection_hex", toHex(observation.pre_raw)},
      {"post_raw_projection_hex", toHex(observation.post_raw)},
      {"canaries", std::move(canaries)},
      {"complete", true},
  };
}

void writeExclusive(const fs::path& path, const std::string& payload)
{
  int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (descriptor < 0)
    throw std::runtime_error("failed to exclusively create result temporary: " + std::string(std::strerror(errno)));
  bool created = true;
  try {
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_uid != ::geteuid())
      throw std::runtime_error("exclusive result temporary is not one current-user regular file");
    std::size_t written = 0;
    while (written < payload.size()) {
      const ssize_t count = ::write(descriptor, payload.data() + written, payload.size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        throw std::runtime_error("failed to write result temporary");
      written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0)
      throw std::runtime_error("failed to synchronize result temporary");
    if (::close(descriptor) != 0)
      throw std::runtime_error("failed to close result temporary");
    descriptor = -1;
    created = false;
  } catch (...) {
    const int saved_errno = errno;
    if (descriptor >= 0)
      static_cast<void>(::close(descriptor));
    if (created)
      static_cast<void>(::unlink(path.c_str()));
    errno = saved_errno;
    throw;
  }
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    if (argc == 2 && std::string_view(argv[1]) == kDifferentClockMapKeySelfTest)
      return runDifferentClockMapKeySelfTest();
    for (int index = 1; index < argc; ++index)
      require(std::string_view(argv[index]) != kDifferentClockMapKeySelfTest, "clock-overlap selftest flag must be the sole argument");

    Options options = parseOptions(argc, argv);
    validateOptions(options);
    Observation observation;
    {
      ScopedOutputSilence silence;
      loadDesign(options);
      std::map<std::string, std::any> config = {{"-temp_directory_path", options.run_dir.string()},
                                                {"-thread_number", std::int32_t{1}},
                                                {"-output_timing_reports", std::int32_t{1}},
                                                {"-output_timing_features", std::int32_t{1}},
                                                {"-timing_path_limit", std::int32_t{20}},
                                                {"-max_paths", std::int32_t{20}},
                                                {"-nworst", std::int32_t{20}},
                                                {"-timing_corner", std::string{}}};
      ista::IncrementalStaSession session({ista::StaBackend::kCpu, ista::StaUpdateStrategy::kFull, ista::StaFallbackPolicy::kError});
      ista::throwOnStaFailure(session.initialize(options.context, config), session.request());
      const ista::StaExecutionRecord record = ista::requireStaExecutionRecord(session.runFullBaseline(), session.request());
      require(record.execution_kind == ista::StaExecutionKind::kFullUpdate && !record.fallback_reason && !record.fallback_to_full,
              "full STA execution record mismatch");
      ista::Database& database = STADM.getDatabase();
      requireFullStaState(options, database);
      observation = observeAndPoison(database, options);
      ista::throwOnStaFailure(session.close(), session.request());
      ista::STAInterface::destroyInst();
    }
    writeExclusive(options.result_temp, makeResult(options, observation).dump(2) + "\n");
    return 0;
  } catch (const std::exception& error) {
    try {
      ista::STAInterface::destroyInst();
    } catch (const std::exception& cleanup_error) {
      std::cerr << "StaCsrReferenceInputProbe cleanup: " << cleanup_error.what() << '\n';
    }
    std::cerr << "StaCsrReferenceInputProbe: " << error.what() << '\n';
    return 1;
  }
}
