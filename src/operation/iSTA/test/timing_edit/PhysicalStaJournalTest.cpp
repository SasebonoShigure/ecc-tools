// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaJournal.hpp"
#include "AllocationFailureSupport.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using ista::PhysicalStaJournal;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F&& action)
{
  try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error("invalid physical recovery action was accepted");
}
struct Directory
{
  std::filesystem::path path;
  Directory()
  {
    std::string pattern = (std::filesystem::temp_directory_path() / "ista-physical-journal-XXXXXX").string();
    auto* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("mkdtemp failed");
    path = created;
  }
  ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
constexpr auto kBaseline = R"({"checkpoint":"fixed-input","rc":"M1/M2"})";
constexpr auto kRecord = R"({"generation":1,"edits":["buffer-1"]})";
void crash(const std::filesystem::path& path, bool publish)
{
  const auto process = ::fork();
  require(process >= 0, "fork failed");
  if (process == 0) {
    try {
      PhysicalStaJournal journal(path, kBaseline);
      auto record = journal.prepare(1, kRecord);
      if (publish) record.publish();
      ::_exit(0);  // Deliberately skip all destructors, like a killed worker.
    } catch (...) { ::_exit(1); }
  }
  int status{};
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) && WEXITSTATUS(status) == 0, "crash worker failed");
}
void systemFailureBoundaries()
{
  Directory retry;
  auto process = ::fork();
  require(process >= 0, "fork failed");
  if (process == 0) {
    try {
      PhysicalStaJournal journal(retry.path, kBaseline);
      struct rlimit original{};
      require(::getrlimit(RLIMIT_NOFILE, &original) == 0, "read fd limit failed");
      auto limited = original;
      limited.rlim_cur = 0;
      require(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "set fd limit failed");
      rejected([&] { auto record = journal.prepare(1, kRecord); });
      require(::setrlimit(RLIMIT_NOFILE, &original) == 0, "restore fd limit failed");
      require(journal.healthy() && journal.generation() == 0, "failed create poisoned the owner");
      auto record = journal.prepare(1, kRecord);
      record.publish();
      require(journal.generation() == 1, "failed create could not be retried");
      ::_exit(0);
    } catch (...) { ::_exit(1); }
  }
  int status{};
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fd exhaustion retry failed");
  Directory fifo;
  require(::mkfifo((fifo.path / "baseline.json").c_str(), 0600) == 0, "mkfifo failed");
  process = ::fork();
  require(process >= 0, "fork failed");
  if (process == 0) {
    ::alarm(3);
    try {
      rejected([&] { PhysicalStaJournal journal(fifo.path, kBaseline); });
      ::_exit(0);
    } catch (...) { ::_exit(1); }
  }
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "non-regular baseline blocked or was accepted");
}
void run()
{
  systemFailureBoundaries();
  Directory directory;
  {
    PhysicalStaJournal journal(directory.path, kBaseline);
    rejected([&] { PhysicalStaJournal other(directory.path, kBaseline); });
    {
      auto abandoned = journal.prepare(1, kRecord);
      rejected([&] { auto second = journal.prepare(1, kRecord); });
    }
    require(journal.generation() == 0 && journal.healthy() && !std::filesystem::exists(directory.path / "pending.json"),
            "abandon published or leaked its prepared record");
    auto record = journal.prepare(1, kRecord);
    ista_test_allocation::armFailure(1);
    record.publish();
    const auto allocations = ista_test_allocation::disable();
    require(allocations == 0 && journal.generation() == 1 && journal.readCommitted(1) == kRecord,
            "publication allocated or did not expose the complete record");
    rejected([&] { record.publish(); });
    rejected([&] { auto stale = journal.prepare(1, kRecord); });
  }
  rejected([&] { PhysicalStaJournal other(directory.path, "different checkpoint"); });
  {
    PhysicalStaJournal journal(directory.path, kBaseline);
    require(journal.generation() == 1 && journal.readCommitted(1) == kRecord, "restart lost the committed record");
    auto record = journal.prepare(2, "new second record");
    std::ofstream(directory.path / "committed-2.json") << "existing second record";
    rejected([&] { record.publish(); });
    require(journal.generation() == 1, "failed rename advanced generation");
    std::ifstream input(directory.path / "committed-2.json");
    std::string line; std::getline(input, line);
    require(line == "existing second record", "rename overwrote an existing generation");
  }
  Directory interrupted;
  crash(interrupted.path, false);
  {
    PhysicalStaJournal journal(interrupted.path, kBaseline);
    require(journal.generation() == 0 && !std::filesystem::exists(interrupted.path / "pending.json"),
            "restart replayed an uncommitted prepare");
  }
  crash(interrupted.path, true);
  {
    PhysicalStaJournal journal(interrupted.path, kBaseline);
    require(journal.generation() == 1 && journal.readCommitted(1) == kRecord, "restart lost a published, unacknowledged edit");
  }
  Directory broken;
  {
    PhysicalStaJournal journal(broken.path, kBaseline);
    {
      auto record = journal.prepare(1, kRecord);
      std::filesystem::remove(broken.path / "pending.json");
      std::filesystem::create_directory(broken.path / "pending.json");
    }
    require(!journal.healthy(), "failed cleanup was hidden");
    rejected([&] { auto record = journal.prepare(1, kRecord); });
  }
  Directory gap;
  {
    PhysicalStaJournal journal(gap.path, kBaseline);
    std::ofstream(gap.path / "committed-2.json") << "gap";
  }
  rejected([&] { PhysicalStaJournal journal(gap.path, kBaseline); });
  std::filesystem::remove(gap.path / "baseline.json");
  rejected([&] { PhysicalStaJournal journal(gap.path, kBaseline); });
  require(!std::filesystem::exists(gap.path / "baseline.json"), "orphan history acquired a false baseline identity");
}
}
int main()
{
  try { run(); std::cout << "physical journal: lock, abandon, no-allocation publish, crash and restart passed\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
