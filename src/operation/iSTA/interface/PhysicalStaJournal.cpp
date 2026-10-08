// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaJournal.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ista {
namespace {
constexpr const char* kPending = "pending.json";
[[noreturn]] void ioFailure(const char* operation)
{
  throw std::system_error(errno, std::generic_category(), operation);
}
std::string recordName(std::uint64_t generation) { return "committed-" + std::to_string(generation) + ".json"; }
void writeNew(int directory, const char* name, std::string_view value, bool& created)
{
  int file = ::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (file < 0) ioFailure("create physical recovery record");
  created = true;
  try {
    while (!value.empty()) {
      const auto written = ::write(file, value.data(), value.size());
      if (written < 0 && errno == EINTR) continue;
      if (written == 0) errno = EIO;
      if (written <= 0) ioFailure("write physical recovery record");
      value.remove_prefix(static_cast<std::size_t>(written));
    }
    const int closing = file;
    file = -1;  // Linux close releases the descriptor even on EINTR.
    if (::close(closing) != 0) ioFailure("close physical recovery record");
  } catch (...) {
    if (file >= 0) ::close(file);
    throw;
  }
}
std::string readFile(int directory, const char* name)
{
  const int file = ::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (file < 0) ioFailure("open physical recovery record");
  std::string result;
  try {
    struct stat status{};
    if (::fstat(file, &status) != 0) ioFailure("inspect physical recovery record");
    if (!S_ISREG(status.st_mode)) throw std::invalid_argument("physical recovery record is not a regular file");
    char buffer[8192];
    for (;;) {
      const auto count = ::read(file, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) ioFailure("read physical recovery record");
      if (count == 0) break;
      result.append(buffer, static_cast<std::size_t>(count));
    }
  } catch (...) { ::close(file); throw; }
  if (::close(file) != 0) ioFailure("close physical recovery input");
  return result;
}
void publishFile(int directory, const char* pending, const char* committed)
{
  // No overwrite, including an existing generation created by a different writer.
  if (::renameat2(directory, pending, directory, committed, RENAME_NOREPLACE) != 0)
    ioFailure("publish physical recovery record");
}
}
PhysicalStaJournal::PhysicalStaJournal(const std::filesystem::path& directory, std::string_view baseline)
{
  if (baseline.empty()) throw std::invalid_argument("physical recovery baseline is empty");
  std::filesystem::create_directories(directory);
  _directory = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (_directory < 0) ioFailure("open physical recovery directory");
  try {
    _lock = ::openat(_directory, "owner.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (_lock < 0) ioFailure("open physical recovery owner");
    if (::flock(_lock, LOCK_EX | LOCK_NB) != 0) ioFailure("lock physical recovery owner");
    if (::faccessat(_directory, "baseline.json", F_OK, 0) != 0) {
      if (errno != ENOENT) ioFailure("inspect physical recovery baseline");
      if (::unlinkat(_directory, "baseline.pending", 0) != 0 && errno != ENOENT)
        ioFailure("remove abandoned physical baseline");
      for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename().string().starts_with("committed-"))
          throw std::invalid_argument("physical recovery records have no baseline");
      bool created = false;
      writeNew(_directory, "baseline.pending", baseline, created);
      publishFile(_directory, "baseline.pending", "baseline.json");
    } else if (readFile(_directory, "baseline.json") != baseline) {
      throw std::invalid_argument("physical recovery checkpoint/configuration differs");
    }
    std::vector<std::uint64_t> generations;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      const auto name = entry.path().filename().string();
      if (!name.starts_with("committed-")) continue;
      if (!entry.is_regular_file() || entry.is_symlink() || !name.ends_with(".json"))
        throw std::invalid_argument("invalid physical recovery record file");
      const auto digits = std::string_view(name).substr(10, name.size() - 15);
      std::uint64_t value{};
      const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || value == 0 || name != recordName(value))
        throw std::invalid_argument("invalid physical recovery generation");
      generations.push_back(value);
    }
    std::sort(generations.begin(), generations.end());
    for (auto value : generations)
      if (_generation == std::numeric_limits<std::uint64_t>::max() || value != ++_generation)
        throw std::invalid_argument("physical recovery generations are not contiguous");
    // A crashed prepare is not a committed edit. The lock excludes live owners.
    if (::unlinkat(_directory, kPending, 0) != 0 && errno != ENOENT)
      ioFailure("remove abandoned physical edit");
  } catch (...) {
    if (_lock >= 0) ::close(_lock);
    ::close(_directory);
    throw;
  }
}
PhysicalStaJournal::~PhysicalStaJournal() noexcept
{
  if (_lock >= 0) ::close(_lock);
  if (_directory >= 0) ::close(_directory);
}
PhysicalStaJournal::Prepared PhysicalStaJournal::prepare(std::uint64_t generation, std::string_view contents)
{
  return Prepared(*this, generation, contents);
}
PhysicalStaJournal::Prepared::Prepared(PhysicalStaJournal& owner, std::uint64_t generation, std::string_view contents)
    : _owner(&owner), _generation(generation), _destination(recordName(generation))
{
  if (owner._pending || !owner.healthy() || owner._generation == std::numeric_limits<std::uint64_t>::max()
      || generation != owner._generation + 1 || contents.empty())
    throw std::logic_error("physical recovery prepare requires the next generation and a healthy owner");
  owner._pending = true;
  bool created = false;
  try { writeNew(owner._directory, kPending, contents, created); }
  catch (...) {
    if (created) {
      if (::unlinkat(owner._directory, kPending, 0) != 0 && errno != ENOENT) owner._cleanup_failed = true;
    } else {
      struct stat status{};
      // A failed create is retryable unless a conflicting pending record exists.
      if (::fstatat(owner._directory, kPending, &status, AT_SYMLINK_NOFOLLOW) == 0) owner._cleanup_failed = true;
    }
    owner._pending = false;
    throw;
  }
}
PhysicalStaJournal::Prepared::~Prepared() noexcept
{
  if (!_owner) return;
  if (::unlinkat(_owner->_directory, kPending, 0) != 0 && errno != ENOENT) _owner->_cleanup_failed = true;
  _owner->_pending = false;
}
void PhysicalStaJournal::Prepared::publish()
{
  if (!_owner || !_owner->healthy()) throw std::logic_error("physical recovery publication is unavailable");
  publishFile(_owner->_directory, kPending, _destination.c_str());
  // The successful rename is the commit point. Only scalar writes follow it.
  _owner->_generation = _generation;
  _owner->_pending = false;
  _owner = nullptr;
}
std::string PhysicalStaJournal::readCommitted(std::uint64_t generation) const
{
  if (!healthy() || generation == 0 || generation > _generation)
    throw std::logic_error("physical recovery read requires a committed generation");
  return readFile(_directory, recordName(generation).c_str());
}
}  // namespace ista
