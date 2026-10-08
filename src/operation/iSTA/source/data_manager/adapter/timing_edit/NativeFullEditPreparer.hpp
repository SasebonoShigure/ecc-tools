// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <set>
#include "NativeEditTransaction.hpp"
#include "ParasiticNet.hpp"
#include "TimingEditResolver.hpp"

namespace ista::csr { struct CsrReferenceInput; }
namespace ista::timing_edit {
class NativeFullEditPreparer;
class NativeFullEditBatch;

// Full rebuilds need stable semantic names, not a second CSR or Arc-slot mirror.
class NativeFullEditState final
{
 public:
  NativeFullEditState(NativeFullEditState&&) = delete;
  NativeFullEditState(const NativeFullEditState&) = delete;
  TransactionGeneration generation() const noexcept { return _generation; }
  const TimingEditResolverState& resolver() const noexcept { return _resolver; }

 private:
  NativeFullEditState(Database& database, TimingEditSessionToken session, std::uint64_t next_instance,
                      TimingEditResolverState resolver, std::set<std::string> clock_nets);
  Database* _database{};
  TimingEditSessionToken _session;
  TransactionGeneration _generation;
  std::uint64_t _next_instance{};
  TimingEditResolverState _resolver;
  std::set<std::string> _clock_nets;
  friend class NativeFullEditPreparer;
  friend class NativeFullEditBatch;
};

class NativeFullEditBatch final
{
 public:
  NativeFullEditBatch(NativeFullEditBatch&& other) noexcept;
  NativeFullEditBatch(const NativeFullEditBatch&) = delete;
  NativeEditTransaction& transaction() noexcept { return _transaction; }
  TransactionGeneration targetGeneration() const noexcept { return _target; }
  [[nodiscard]] bool canCommit() const noexcept;
  [[nodiscard]] bool commit() noexcept;

 private:
  NativeFullEditBatch(NativeFullEditState& state, TransactionGeneration target, std::uint64_t next_instance,
                     PreparedTimingEditResolverPatch patch, NativeEditTransaction transaction);
  NativeFullEditState* _state;
  TransactionGeneration _target;
  std::uint64_t _next_instance;
  // Destruction restores native values before releasing resolver preparation.
  PreparedTimingEditResolverPatch _patch;
  NativeEditTransaction _transaction;
  friend class NativeFullEditPreparer;
};

class NativeFullEditPreparer final
{
 public:
  static NativeFullEditState bindBaseline(Database& database, const csr::CsrReferenceInput& input,
                                          const BaselineSemanticIdentity& baseline);
  static NativeFullEditBatch prepare(Database& database, const NativeExecutionAuthority& authority,
                                      EditBatch edits, NativeFullEditState& state,
                                      std::map<std::string, ParasiticNet> prepared_rc = {});
};
}  // namespace ista::timing_edit
