// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrTargetedArtifactSupport.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include "CsrFingerprint.hpp"
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "BufferTraceTypes.hpp"

namespace ista::timing_dual_csr::test {
namespace {

namespace fs = std::filesystem;
using buffer_trace::SourceArtifactFunctionalRecord;
using buffer_trace::SourceArtifactRefs;
using timing_edit::oracle::FreshBaselineArtifacts;

std::atomic<std::uint64_t> g_temporary_ordinal{0};

[[noreturn]] void artifactFail(std::string message)
{
  throw std::runtime_error("Stage 5 targeted artifacts: "
                           + std::move(message));
}

fs::path makeOwnedTemporaryDirectory()
{
  std::error_code error;
  const fs::path root = fs::temp_directory_path(error);
  if (error || root.empty())
    artifactFail("system temporary directory is unavailable");
  const auto epoch
      = std::chrono::steady_clock::now().time_since_epoch().count();
  for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
    const std::uint64_t ordinal
        = g_temporary_ordinal.fetch_add(1, std::memory_order_relaxed);
    const fs::path candidate
        = root / ("ista-dual-csr-targeted-" + std::to_string(epoch)
                  + "-" + std::to_string(ordinal));
    if (fs::create_directory(candidate, error))
      return candidate;
    if (error && error != std::errc::file_exists)
      artifactFail("cannot create an isolated artifact directory");
    error.clear();
  }
  artifactFail("cannot allocate a unique isolated artifact directory");
}

std::string requireLeafName(const std::string& name,
                            std::string_view role)
{
  const fs::path path(name);
  if (name.empty() || name.find('\0') != std::string::npos
      || path.filename() != path || path == "." || path == "..")
    artifactFail(std::string(role)
                 + " logical_name must be one safe leaf name");
  return name;
}

void writeArtifact(const fs::path& path, const std::string& source)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    artifactFail("cannot open a synthetic source artifact");
  output.write(source.data(), static_cast<std::streamsize>(source.size()));
  output.close();
  if (!output)
    artifactFail("cannot write a synthetic source artifact");
}

SourceArtifactRefs materializeSynthetic(
    const buffer_trace::SyntheticArtifactRecipe& recipe,
    const fs::path& directory)
{
  const std::array<std::string, 5> names{
      requireLeafName(recipe.tech_lef.logical_name, "tech_lef"),
      requireLeafName(recipe.cell_lef.logical_name, "cell_lef"),
      requireLeafName(recipe.liberty.logical_name, "liberty"),
      requireLeafName(recipe.verilog.logical_name, "verilog"),
      requireLeafName(recipe.sdc.logical_name, "sdc")};
  if (std::set<std::string>(names.begin(), names.end()).size()
      != names.size())
    artifactFail("five synthetic roles require distinct paths");
  const fs::path tech = directory / names[0];
  const fs::path cell = directory / names[1];
  const fs::path liberty = directory / names[2];
  const fs::path verilog = directory / names[3];
  const fs::path sdc = directory / names[4];
  writeArtifact(tech, recipe.tech_lef.source_text);
  writeArtifact(cell, recipe.cell_lef.source_text);
  writeArtifact(liberty, recipe.liberty.source_text);
  writeArtifact(verilog, recipe.verilog.source_text);
  writeArtifact(sdc, recipe.sdc.source_text);
  return {{tech.string()}, {{cell.string()}}, {{liberty.string()}},
          {verilog.string()}, {sdc.string()}, recipe.top, {"slow", "25", "__no_rc_corner__"}};
}

SourceArtifactRefs sourceRefs(
    const buffer_trace::ReplayProgram& program,
    const fs::path& temporary_directory)
{
  if (const auto* synthetic
      = std::get_if<buffer_trace::SyntheticReplayPayload>(
          &program.workload))
    return materializeSynthetic(
        synthetic->source_recipe, temporary_directory);
  return std::get<buffer_trace::RealReplayPayload>(
             program.workload)
      .artifacts;
}

std::vector<fs::path> canonicalArtifactPaths(
    const SourceArtifactRefs& refs)
{
  std::vector<fs::path> supplied{refs.tech_lef.path};
  for (const auto& source : refs.cell_lefs)
    supplied.emplace_back(source.path);
  for (const auto& source : refs.liberties)
    supplied.emplace_back(source.path);
  supplied.emplace_back(refs.verilog.path);
  supplied.emplace_back(refs.sdc.path);
  std::vector<fs::path> canonical(supplied.size());
  std::set<fs::path> unique;
  for (std::size_t index = 0; index < supplied.size(); ++index) {
    std::error_code error;
    const auto status = fs::symlink_status(supplied[index], error);
    if (error || status.type() != fs::file_type::regular)
      artifactFail("each source must be a regular non-symlink file");
    canonical[index] = fs::canonical(supplied[index], error);
    if (error || !canonical[index].is_absolute()
        || !unique.insert(canonical[index]).second)
      artifactFail("source artifacts require distinct canonical paths");
  }
  return canonical;
}

FreshBaselineArtifacts productionArtifacts(
    const SourceArtifactRefs& refs, const fs::path& root)
{
  if (refs.top.empty() || refs.top.find('\0') != std::string::npos)
    artifactFail("source top name is empty or contains NUL");
  const auto paths = canonicalArtifactPaths(refs);
  const fs::path run_directory = root / "run";
  std::error_code error;
  if (!fs::create_directory(run_directory, error) || error)
    artifactFail("cannot create the isolated production run directory");
  FreshBaselineArtifacts result{paths.front(), {}, {}, paths[paths.size() - 2], paths.back(),
                                run_directory, refs.top, refs.analysis_context};
  std::size_t index = 1;
  for (std::size_t count = 0; count < refs.cell_lefs.size(); ++count)
    result.cell_lefs.push_back(paths[index++]);
  for (std::size_t count = 0; count < refs.liberties.size(); ++count)
    result.liberties.push_back(paths[index++]);
  return result;
}

std::vector<SourceArtifactFunctionalRecord> captureArtifacts(
    const FreshBaselineArtifacts& artifacts)
{
  const auto files = timing_edit::oracle::freshArtifactFiles(artifacts);
  std::vector<SourceArtifactFunctionalRecord> result;
  result.reserve(files.size());
  for (std::size_t index = 0; index < files.size(); ++index) {
    std::error_code error;
    const fs::path canonical = fs::canonical(files[index].path, error);
    std::ifstream input(canonical, std::ios::binary);
    if (error || !input)
      artifactFail("source artifact disappeared during guarded work");
    const std::uintmax_t size = fs::file_size(canonical, error);
    if (error || size > static_cast<std::uintmax_t>(
                            std::numeric_limits<std::streamsize>::max()))
      artifactFail("source artifact size cannot be captured exactly");
    std::string digest;
    try {
      digest = csr::sha256Hex(input, size);
    } catch (const std::exception& error) {
      artifactFail(error.what());
    }
    result.push_back(
        {files[index].role, files[index].logical_name,
         canonical.string(), static_cast<std::uint64_t>(size),
         digest});
  }
  return result;
}

}  // namespace

class DualCsrTargetedArtifactSupport::Impl final
{
 public:
  explicit Impl(const buffer_trace::ReplayProgram& program)
      : root(makeOwnedTemporaryDirectory()),
        bundle(productionArtifacts(sourceRefs(program, root), root))
  {
  }

  ~Impl()
  {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }

  fs::path root;
  FreshBaselineArtifacts bundle;
};

DualCsrTargetedArtifactSupport::DualCsrTargetedArtifactSupport(
    const buffer_trace::ReplayProgram& program)
    : _impl(std::make_unique<Impl>(program))
{
}

DualCsrTargetedArtifactSupport::~DualCsrTargetedArtifactSupport()
    = default;

void DualCsrTargetedArtifactSupport::close()
{
  fs::remove_all(_impl->root);
}

const FreshBaselineArtifacts&
DualCsrTargetedArtifactSupport::artifacts() const noexcept
{
  return _impl->bundle;
}

std::vector<SourceArtifactFunctionalRecord>
DualCsrTargetedArtifactSupport::capture() const
{
  return captureArtifacts(_impl->bundle);
}

}  // namespace ista::timing_dual_csr::test
