// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include <memory>
#include <span>
#include "TimingEditTypes.hpp"

namespace ista {
class Database;
namespace timing_edit {
class NativeExecutionAuthority;
class NativeExecutionScopeState;
class NativeCandidateExecutor;
class TimingEditResolverView;
namespace detail {
struct PreparedNativeBufferInsertions;
struct PreparedNativeGraphPatch;
struct PreparedNativeTargetedMetadata;
}
enum class NativeRollbackFailure : std::uint8_t;
enum class NativeRollbackFailpoint : std::uint8_t;

// One guarded edit on the canonical Database. The undo owns local raw old
// values and the old derived graph, never a second Database or Liberty.
class NativeEditTransaction final
{
 public:
  NativeEditTransaction(const NativeEditTransaction&) = delete;
  NativeEditTransaction& operator=(const NativeEditTransaction&) = delete;
  NativeEditTransaction(NativeEditTransaction&&) noexcept;
  NativeEditTransaction& operator=(NativeEditTransaction&&) = delete;
  ~NativeEditTransaction();

  [[nodiscard]] static NativeEditTransaction prepareFull(const NativeExecutionAuthority& authority);
  [[nodiscard]] static NativeEditTransaction prepareFull(
      const NativeExecutionAuthority& authority, const PreparedBatch& batch,
      TimingEditResolverView resolver);
  [[nodiscard]] Database& database();
  [[nodiscard]] const Database& database() const;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool rollback() noexcept;
  [[nodiscard]] NativeRollbackFailure rollbackStatus() noexcept;
  void commit() noexcept;

 private:
  friend class NativeFullEditPreparer;
  static NativeEditTransaction prepareFull(const NativeExecutionAuthority& authority,
      std::span<const BufferInsertionMutation> edits, TimingEditResolverView resolver, std::uint64_t first_instance_id);
  struct Undo;
  [[nodiscard]] static NativeEditTransaction beginTargeted(const NativeExecutionAuthority& authority);
  void applyMetadata(std::unique_ptr<detail::PreparedNativeTargetedMetadata> metadata) noexcept;
  void applyTargeted(detail::PreparedNativeBufferInsertions&& raw,
                     detail::PreparedNativeGraphPatch&& graph) noexcept;
  explicit NativeEditTransaction(std::unique_ptr<Undo> undo) noexcept;
  void attach(const NativeExecutionAuthority& authority);
  void apply() noexcept;
  void undo() noexcept;
  void discardUndo() noexcept;
  [[nodiscard]] NativeRollbackFailure rollbackWithDiagnostics(NativeRollbackFailpoint failpoint) noexcept;

  std::unique_ptr<Undo> _undo;
  std::shared_ptr<NativeExecutionScopeState> _execution_scope;
  TimingEditSessionToken _session_token{};
  bool _attached{false};
  bool _applied{false};
  friend class NativeCandidateExecutor;
  friend class NativeTargetedEditPreparer;
};
}  // namespace timing_edit
}  // namespace ista
