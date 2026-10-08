// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ista {
// One locked directory per physical checkpoint. Records are complete before
// rename; no fsync durability is promised across a host/power failure.
class PhysicalStaJournal final
{
 public:
  class Prepared final
  {
   public:
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    ~Prepared() noexcept;
    void publish();
   private:
    friend class PhysicalStaJournal;
    Prepared(PhysicalStaJournal& owner, std::uint64_t generation, std::string_view contents);
    PhysicalStaJournal* _owner;
    std::uint64_t _generation;
    std::string _destination;
  };
  PhysicalStaJournal(const std::filesystem::path& directory, std::string_view baseline);
  ~PhysicalStaJournal() noexcept;
  PhysicalStaJournal(const PhysicalStaJournal&) = delete;
  PhysicalStaJournal& operator=(const PhysicalStaJournal&) = delete;
  [[nodiscard]] Prepared prepare(std::uint64_t generation, std::string_view contents);
  [[nodiscard]] std::string readCommitted(std::uint64_t generation) const;
  [[nodiscard]] std::uint64_t generation() const noexcept { return _generation; }
  [[nodiscard]] bool healthy() const noexcept { return !_cleanup_failed; }
 private:
  int _directory{-1}, _lock{-1};
  std::uint64_t _generation{};
  bool _pending{false}, _cleanup_failed{false};
};
}  // namespace ista
