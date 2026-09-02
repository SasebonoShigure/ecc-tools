// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "CsrFingerprint.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferencePoisonSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;
void require(bool condition, std::string_view message)
{
  if (!condition)
    throw std::runtime_error(std::string(message));
}

class Reader
{
 public:
  explicit Reader(const std::vector<std::uint8_t>& bytes) : _bytes(bytes) {}
  std::uint8_t u8()
  {
    require(_offset < _bytes.size(), "truncated poison canary");
    return _bytes[_offset++];
  }
  std::uint32_t u32()
  {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index)
      value = (value << 8) | u8();
    return value;
  }
  std::uint64_t u64()
  {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
      value = (value << 8) | u8();
    return value;
  }
  std::size_t count()
  {
    const std::uint64_t value = u64();
    require(value <= _bytes.size() - _offset && value <= std::numeric_limits<std::size_t>::max(), "poison count is not bounded");
    return static_cast<std::size_t>(value);
  }
  std::string string()
  {
    const std::size_t size = count();
    const auto* begin = reinterpret_cast<const char*>(_bytes.data() + _offset);
    _offset += size;
    return std::string(begin, size);
  }
  std::string boolean()
  {
    const std::uint8_t value = u8();
    require(value <= 1, "poison boolean is invalid");
    return "b:" + std::to_string(value);
  }
  std::string i32() { return "i:" + std::to_string(std::bit_cast<std::int32_t>(u32())); }
  std::string integer() { return "u:" + std::to_string(u64()); }
  std::string enumeration(std::uint8_t maximum)
  {
    const std::uint8_t value = u8();
    require(value <= maximum, "poison enum " + std::to_string(value) + " exceeds " + std::to_string(maximum));
    return "e:" + std::to_string(value);
  }
  std::string number()
  {
    const std::uint8_t tag = u8();
    if (tag == 1)
      return "d:+inf";
    if (tag == 2)
      return "d:-inf";
    require(tag == 0, "poison double tag is invalid");
    const std::uint64_t bits = u64();
    const double value = std::bit_cast<double>(bits);
    require(std::isfinite(value) && !(value == 0.0 && std::signbit(value)), "poison finite double is not canonical");
    return "d:" + std::to_string(bits);
  }
  void finish() const { require(_offset == _bytes.size(), "poison canary has trailing bytes"); }

 private:
  std::span<const std::uint8_t> _bytes;
  std::size_t _offset{0};
};

std::string stableKey(const std::vector<std::string>& fields)
{
  std::string result;
  for (const auto& field : fields)
    result += std::to_string(field.size()) + ":" + field;
  return result;
}

struct DecodedSubject
{
  std::string key;
  std::vector<std::string> shape;
  std::vector<std::string> required;
  std::vector<std::string> invariant;
  friend bool operator==(const DecodedSubject&, const DecodedSubject&) = default;
};
enum class LeafKind : std::uint8_t
{
  kNumber,
  kString,
  kTransition
};
std::string leaf(Reader& reader, LeafKind kind)
{
  switch (kind) {
    case LeafKind::kNumber:
      return reader.number();
    case LeafKind::kString:
      return "s:" + reader.string();
    case LeafKind::kTransition:
      return reader.enumeration(2);
  }
  throw std::runtime_error("unknown poison leaf kind");
}

void appendAnalysisTransitionMap(Reader& reader, LeafKind kind, std::string_view label, DecodedSubject& subject)
{
  const std::size_t count = reader.count();
  std::set<std::string> keys;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string analysis = reader.enumeration(1);
    const std::string transition = reader.enumeration(2);
    const std::string key = std::string(label) + "/" + analysis + "/" + transition;
    require(keys.insert(key).second, "analysis/transition map key is duplicated");
    subject.shape.push_back(key);
    subject.required.push_back(leaf(reader, kind));
  }
}

void appendPairedMaps(Reader& reader, LeafKind kind, DecodedSubject& subject)
{
  appendAnalysisTransitionMap(reader, kind, "effective", subject);
  appendAnalysisTransitionMap(reader, kind, "physical", subject);
}

void appendInputOutputMap(Reader& reader, DecodedSubject& subject)
{
  const std::size_t count = reader.count();
  std::set<std::string> keys;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string analysis = reader.enumeration(1);
    const std::string input = reader.enumeration(2);
    const std::string output = reader.enumeration(2);
    const std::string key = "io/" + analysis + "/" + input + "/" + output;
    require(keys.insert(key).second, "input/output map key is duplicated");
    subject.shape.push_back(key);
    subject.required.push_back(reader.number());
  }
}

void appendTimingArcMap(Reader& reader, DecodedSubject& subject)
{
  const std::size_t count = reader.count();
  std::set<std::string> keys;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string arc_index = reader.i32();
    const std::string analysis = reader.enumeration(1);
    const std::string input = reader.enumeration(2);
    const std::string output = reader.enumeration(2);
    const std::string key = "ta/" + arc_index + "/" + analysis + "/" + input + "/" + output;
    require(keys.insert(key).second, "timing-arc map key is duplicated");
    subject.shape.push_back(key);
    subject.required.push_back(reader.number());
  }
}

void appendPathStates(Reader& reader, DecodedSubject& subject)
{
  const std::size_t count = reader.count();
  std::set<std::string> keys;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string analysis = reader.enumeration(1);
    const std::string source = reader.enumeration(1);
    const std::string transition = reader.enumeration(2);
    const std::string clock = reader.string();
    const std::string key = "path/" + analysis + "/" + source + "/" + transition + "/" + stableKey({clock});
    require(keys.insert(key).second, "path-state map key is duplicated");
    subject.shape.push_back(key);
    for (int leaf_index = 0; leaf_index < 3; ++leaf_index)
      subject.required.push_back(reader.number());
    for (int leaf_index = 0; leaf_index < 4; ++leaf_index)
      subject.required.push_back("s:" + reader.string());
    subject.required.push_back(reader.integer());
    subject.required.push_back(reader.number());
    for (int leaf_index = 0; leaf_index < 3; ++leaf_index)
      subject.required.push_back(reader.enumeration(2));
  }
}

std::string appendArcStableKey(Reader& reader)
{
  return stableKey({reader.integer(), reader.string(), reader.string(), reader.enumeration(1), reader.string(), reader.string(), reader.string()});
}

DecodedSubject decodeSubject(Reader& reader, std::size_t family)
{
  DecodedSubject subject;
  if (family <= 9)
    subject.key = stableKey({reader.string()});
  else if (family <= 15)
    subject.key = appendArcStableKey(reader);
  else {
    const std::string key = reader.string();
    require(key == "database", "database poison subject key differs");
    subject.key = key;
  }

  switch (family) {
    case 0:
      subject.required.push_back(reader.i32());
      break;
    case 1:
      subject.required.push_back(reader.boolean());
      break;
    case 2:
      for (int index = 0; index < 4; ++index)
        subject.required.push_back(reader.number());
      subject.required.push_back("s:" + reader.string());
      subject.required.push_back("s:" + reader.string());
      subject.required.push_back(reader.integer());
      break;
    case 3:
    case 4:
    case 6:
      appendPairedMaps(reader, LeafKind::kNumber, subject);
      break;
    case 5:
      appendPairedMaps(reader, LeafKind::kString, subject);
      break;
    case 7:
      appendPairedMaps(reader, LeafKind::kTransition, subject);
      break;
    case 8:
      appendAnalysisTransitionMap(reader, LeafKind::kNumber, "data", subject);
      break;
    case 9:
      appendPathStates(reader, subject);
      break;
    case 10:
      for (int index = 0; index < 3; ++index)
        subject.required.push_back(reader.number());
      break;
    case 11: {
      appendAnalysisTransitionMap(reader, LeafKind::kNumber, "delay", subject);
      const std::size_t count = reader.count();
      std::set<std::string> keys;
      for (std::size_t index = 0; index < count; ++index) {
        const std::string key = "transition/" + reader.enumeration(2);
        require(keys.insert(key).second, "transition map key is duplicated");
        subject.shape.push_back(key);
        subject.required.push_back(reader.enumeration(2));
      }
      break;
    }
    case 12:
    case 13:
      appendInputOutputMap(reader, subject);
      break;
    case 14:
      appendTimingArcMap(reader, subject);
      break;
    case 15:
      subject.invariant.push_back(reader.boolean());
      subject.invariant.push_back(reader.boolean());
      subject.required.push_back(reader.boolean());
      break;
    case 16: {
      const std::size_t count = reader.count();
      require(count != 0, "database timing order is empty");
      std::vector<std::string> order;
      order.reserve(count);
      for (std::size_t index = 0; index < count; ++index)
        order.push_back(reader.string());
      subject.shape.push_back("count:" + std::to_string(count));
      subject.required.push_back(stableKey(order));
      break;
    }
    default:
      throw std::runtime_error("unknown poison family");
  }
  return subject;
}

struct DecodedCanary
{
  std::string family;
  std::uint64_t repeat_index{0};
  std::string run_nonce;
  std::uint64_t session_token{0};
  std::vector<DecodedSubject> subjects;

  friend bool operator==(const DecodedCanary&, const DecodedCanary&) = default;
};

DecodedCanary decodeCanary(const std::vector<std::uint8_t>& bytes, std::size_t family)
{
  Reader reader(bytes);
  DecodedCanary result;
  result.family = std::string(kReferencePoisonFamilyTokens.at(family));
  require(reader.string() == "ista-csr-reference-input-canary/" + result.family + "/1.0.0", "poison domain differs");
  result.repeat_index = reader.u64();
  result.run_nonce = reader.string();
  result.session_token = reader.u64();
  const std::size_t count = reader.count();
  require(count != 0, "poison family has no subjects");
  result.subjects.reserve(count);
  std::set<std::string> keys;
  for (std::size_t index = 0; index < count; ++index) {
    DecodedSubject subject = decodeSubject(reader, family);
    require(keys.insert(subject.key).second, "poison subject is duplicated");
    result.subjects.push_back(std::move(subject));
  }
  reader.finish();
  return result;
}
std::string finiteNumber(double value)
{
  return "d:" + std::to_string(std::bit_cast<std::uint64_t>(value));
}
bool hasShapePrefix(const DecodedSubject& subject, std::string_view prefix)
{
  return std::any_of(subject.shape.begin(), subject.shape.end(), [&](const std::string& shape) { return shape.starts_with(prefix); });
}
void addLeafIfMapEmpty(DecodedSubject& subject, std::string_view prefix, std::string key, std::string value, bool front)
{
  if (hasShapePrefix(subject, prefix))
    return;
  const std::size_t at = front ? 0 : subject.shape.size();
  subject.shape.insert(subject.shape.begin() + static_cast<std::ptrdiff_t>(at), std::move(key));
  subject.required.insert(subject.required.begin() + static_cast<std::ptrdiff_t>(at), std::move(value));
}
void scaffoldPair(DecodedSubject& subject, std::string effective, std::string physical)
{
  addLeafIfMapEmpty(subject, "effective/", "effective/e:0/e:0", std::move(effective), true);
  addLeafIfMapEmpty(subject, "physical/", "physical/e:0/e:0", std::move(physical), false);
}
std::int32_t timingIndex(const std::string& shape)
{
  return static_cast<std::int32_t>(std::stoll(shape.substr(5, shape.find('/', 5) - 5)));
}
void scaffoldSubject(DecodedSubject& subject, std::size_t family)
{
  switch (family) {
    case 3:
      scaffoldPair(subject, finiteNumber(1.0), finiteNumber(2.0));
      break;
    case 4:
      scaffoldPair(subject, finiteNumber(3.0), finiteNumber(4.0));
      break;
    case 5:
      scaffoldPair(subject, "s:__csr_predecessor__", "s:__csr_physical_predecessor__");
      break;
    case 6:
      scaffoldPair(subject, finiteNumber(5.0), finiteNumber(6.0));
      break;
    case 7:
      scaffoldPair(subject, "e:0", "e:0");
      break;
    case 8:
      addLeafIfMapEmpty(subject, "data/", "data/e:0/e:0", finiteNumber(7.0), false);
      break;
    case 9:
      if (subject.shape.empty()) {
        subject.shape = {"path/e:0/e:0/e:0/" + stableKey({"__csr_poison_clock__"})};
        subject.required = {"d:-inf",
                            finiteNumber(0.0),
                            finiteNumber(0.0),
                            "s:",
                            "s:",
                            "s:",
                            "s:",
                            "u:" + std::to_string(std::numeric_limits<std::uint64_t>::max()),
                            finiteNumber(0.0),
                            "e:2",
                            "e:2",
                            "e:2"};
      }
      break;
    case 11:
      addLeafIfMapEmpty(subject, "delay/", "delay/e:0/e:0", finiteNumber(1.0), true);
      addLeafIfMapEmpty(subject, "transition/", "transition/e:0", "e:0", false);
      break;
    case 12:
    case 13:
      addLeafIfMapEmpty(subject, "io/", "io/e:0/e:0/e:0", finiteNumber(family == 12 ? 2.0 : 3.0), false);
      break;
    case 14: {
      if (subject.shape.empty()) {
        subject.shape.push_back("ta/i:0/e:0/e:0/e:0");
        subject.required.push_back(finiteNumber(4.0));
      }
      std::int32_t key = std::numeric_limits<std::int32_t>::min();
      while (hasShapePrefix(subject, "ta/i:" + std::to_string(key) + "/")) {
        require(key != std::numeric_limits<std::int32_t>::max(), "scaffold fallback index exhausted");
        ++key;
      }
      std::size_t at = 0;
      while (at < subject.shape.size() && timingIndex(subject.shape[at]) < key)
        ++at;
      subject.shape.insert(subject.shape.begin() + static_cast<std::ptrdiff_t>(at), "ta/i:" + std::to_string(key) + "/e:0/e:0/e:0");
      subject.required.insert(subject.required.begin() + static_cast<std::ptrdiff_t>(at), finiteNumber(40.0));
      break;
    }
    default:
      break;
  }
}
void validateScaffold(const DecodedCanary& pristine, const DecodedCanary& before, std::size_t family)
{
  require(pristine.family == before.family && pristine.repeat_index == before.repeat_index && pristine.run_nonce == before.run_nonce
              && pristine.session_token == before.session_token && pristine.subjects.size() == before.subjects.size(),
          "pristine/scaffold binding or subject inventory drifted");
  for (std::size_t index = 0; index < pristine.subjects.size(); ++index) {
    DecodedSubject expected = pristine.subjects[index];
    scaffoldSubject(expected, family);
    require(expected == before.subjects[index], "scaffold differs from exact first-legal defaults");
  }
}
enum class RequiredDeltaMode : std::uint8_t
{
  kEveryLeaf,
  kExactlyOneLeaf
};

void validateRequiredDelta(const DecodedCanary& before, const DecodedCanary& after, RequiredDeltaMode mode)
{
  require(before.family == after.family && before.repeat_index == after.repeat_index && before.run_nonce == after.run_nonce
              && before.session_token == after.session_token,
          "poison family binding drifted");
  require(before.subjects.size() == after.subjects.size(), "poison subject count drifted");
  std::size_t changed_leaves = 0;
  std::size_t required_leaves = 0;
  for (std::size_t index = 0; index < before.subjects.size(); ++index) {
    const auto& old = before.subjects[index];
    const auto& changed = after.subjects[index];
    require(old.key == changed.key, "poison subject key drifted");
    require(old.shape == changed.shape, "poison map shape drifted");
    require(old.invariant == changed.invariant, "poison negative-control leaf changed");
    require(old.required.size() == changed.required.size() && !old.required.empty(), "poison required-leaf shape drifted");
    for (std::size_t leaf_index = 0; leaf_index < old.required.size(); ++leaf_index) {
      ++required_leaves;
      changed_leaves += old.required[leaf_index] != changed.required[leaf_index];
    }
  }
  require(required_leaves != 0, "poison family has no required leaves");
  if (mode == RequiredDeltaMode::kEveryLeaf)
    require(changed_leaves == required_leaves, "full-family poison omitted a required leaf");
  else
    require(changed_leaves == 1, "one-leaf classifier case is not an exact single delta");
}

void validatePair(const DecodedCanary& before, const DecodedCanary& after)
{
  validateRequiredDelta(before, after, RequiredDeltaMode::kEveryLeaf);
}

std::vector<std::string> expectedSubjects(const CsrReferenceInputRawProjection& raw, std::size_t family)
{
  std::vector<std::string> result;
  if (family <= 9) {
    for (const RawNode& node : raw.nodes)
      result.push_back(stableKey({node.full_name}));
  } else if (family <= 15) {
    for (const RawArc& arc : raw.arcs)
      result.push_back(stableKey({"u:" + std::to_string(arc.native_rank), arc.source, arc.sink, "e:" + std::to_string(static_cast<std::uint8_t>(arc.kind)),
                                  arc.owner, arc.library_source_port, arc.library_sink_port}));
  } else {
    result.push_back("database");
  }
  return result;
}

void requireRejected(const DecodedCanary& before, const DecodedCanary& after)
{
  bool rejected = false;
  try {
    validatePair(before, after);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "incomplete poison pair was accepted");
}
std::uint32_t classifySingleLeafDelta(const std::vector<DecodedCanary>& before, const std::vector<DecodedCanary>& after)
{
  require(before.size() == kReferencePoisonFamilyTokens.size() && after.size() == before.size(), "orthogonal classifier family inventory differs");
  std::uint32_t mask = 0;
  for (std::size_t family = 0; family < before.size(); ++family) {
    if (before[family] == after[family])
      continue;
    validateRequiredDelta(before[family], after[family], RequiredDeltaMode::kExactlyOneLeaf);
    mask |= std::uint32_t{1} << family;
  }
  require(std::has_single_bit(mask), "orthogonal classifier accepted no-op or cross-family spill");
  return mask;
}

void requireClassifierRejected(const std::vector<DecodedCanary>& before, const std::vector<DecodedCanary>& after)
{
  bool rejected = false;
  try {
    static_cast<void>(classifySingleLeafDelta(before, after));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "orthogonal classifier accepted a forbidden delta pattern");
}

void verifyOrthogonalClassifierMatrix(const std::vector<DecodedCanary>& before, const std::vector<DecodedCanary>& full_after)
{
  require(before.size() == kReferencePoisonFamilyTokens.size() && full_after.size() == before.size(), "orthogonal matrix family inventory differs");
  for (std::size_t family = 0; family < before.size(); ++family) {
    for (std::size_t subject = 0; subject < before[family].subjects.size(); ++subject) {
      for (std::size_t leaf_index = 0; leaf_index < before[family].subjects[subject].required.size(); ++leaf_index) {
        std::vector<DecodedCanary> one_leaf = before;
        one_leaf[family].subjects[subject].required[leaf_index] = full_after[family].subjects[subject].required[leaf_index];
        for (std::size_t other = 0; other < before.size(); ++other)
          if (other != family)
            require(one_leaf[other] == before[other], "orthogonal case changed another family's decoded canonical form");
        require(classifySingleLeafDelta(before, one_leaf) == (std::uint32_t{1} << family), "required leaf did not classify to exactly its owning family");
      }
    }
  }

  requireClassifierRejected(before, before);
  std::vector<DecodedCanary> spill = before;
  for (std::size_t family = 0; family < 2; ++family)
    spill[family].subjects.front().required.front() = full_after[family].subjects.front().required.front();
  requireClassifierRejected(before, spill);
}
CsrReferenceInput exportInput(const ista::Database& database, std::uint64_t token)
{
  CsrReferenceInputExportResult result = exportCsrReferenceInputCandidate(database, referenceInputTestContext(), token);
  require(std::holds_alternative<CsrReferenceInput>(result), "poison fixture export returned a typed failure");
  CsrReferenceInput input = std::get<CsrReferenceInput>(std::move(result));
  require(validateReferenceInputForSession(input, token).empty(), "poison fixture export is not strict-valid");
  return input;
}
void verifyCanaries(const std::vector<ReferencePoisonCanary>& canaries, const CsrReferenceInputRawProjection& raw, const ReferencePoisonBinding& binding)
{
  require(canaries.size() == kReferencePoisonFamilyTokens.size(), "poison family count differs");
  std::vector<DecodedCanary> decoded_before;
  std::vector<DecodedCanary> decoded_after;
  decoded_before.reserve(canaries.size());
  decoded_after.reserve(canaries.size());
  std::uint32_t touched_mask = 0;
  for (std::size_t family = 0; family < canaries.size(); ++family) {
    const auto& canary = canaries[family];
    require(canary.family == kReferencePoisonFamilyTokens[family] && !canary.pristine_typed_bytes.empty() && !canary.before_typed_bytes.empty()
                && !canary.after_typed_bytes.empty() && canary.before_typed_bytes != canary.after_typed_bytes,
            "poison family token/order/bytes differ");
    DecodedCanary before;
    DecodedCanary pristine;
    DecodedCanary after;
    try {
      before = decodeCanary(canary.before_typed_bytes, family);
      pristine = decodeCanary(canary.pristine_typed_bytes, family);
      after = decodeCanary(canary.after_typed_bytes, family);
    } catch (const std::runtime_error& error) {
      throw std::runtime_error(canary.family + ": " + error.what());
    }
    validateScaffold(pristine, before, family);
    if (family <= 2 || family == 10 || family >= 15)
      require(canary.pristine_typed_bytes == canary.before_typed_bytes, "non-scaffold family changed between pristine and before");

    require(before.repeat_index == binding.repeat_index && before.run_nonce == binding.run_nonce && before.session_token == binding.session_token,
            "poison binding was not encoded exactly");
    std::vector<std::string> actual_subjects;
    for (const auto& subject : before.subjects)
      actual_subjects.push_back(subject.key);
    require(actual_subjects == expectedSubjects(raw, family), "poison subjects differ from raw stable inventory");
    validatePair(before, after);
    touched_mask |= std::uint32_t{1} << family;

    for (std::size_t subject_index = 0; subject_index < after.subjects.size(); ++subject_index)
      for (std::size_t leaf_index = 0; leaf_index < after.subjects[subject_index].required.size(); ++leaf_index) {
        DecodedCanary incomplete = after;
        incomplete.subjects[subject_index].required[leaf_index] = before.subjects[subject_index].required[leaf_index];
        requireRejected(before, incomplete);
      }

    DecodedCanary bad_shape = after;
    bad_shape.subjects.front().shape.push_back("extra-shape");
    requireRejected(before, bad_shape);
    if (family == 15) {
      for (const auto& subject : before.subjects) {
        require(subject.invariant == std::vector<std::string>{"b:0", "b:0"}, "ordinary min/max fallback control must stay false");
        require(subject.required == std::vector<std::string>{"b:1"}, "fallback provenance before behavior must replace");
      }
      for (const auto& subject : after.subjects)
        require(subject.required == std::vector<std::string>{"b:0"}, "fallback provenance poison did not consume replacement");
      for (std::size_t control = 0; control < 2; ++control) {
        DecodedCanary bad_control = after;
        bad_control.subjects.front().invariant[control] = "b:1";
        requireRejected(before, bad_control);
      }
    }
    decoded_before.push_back(std::move(before));
    decoded_after.push_back(std::move(after));
  }
  require(touched_mask == 0x1ffffU, "aggregate poison touched mask is not exact");
  verifyOrthogonalClassifierMatrix(decoded_before, decoded_after);
}
// Synthetic derived-state fixture setup; production/full-STA timing order is never fabricated here.
void installSyntheticTimingOrder(ista::Database& database)
{
  auto& order = database.get_timing_order_list();
  if (database.get_timing_point_map().empty()) {
    require(order.empty(), "empty graph timing order is not a no-op");
    return;
  }
  require(order.empty(), "synthetic timing order was already populated");
  order = {"clk", "din", "ff0:CK", "ff0:D", "ff0:Q", "ff1:CK", "ff1:D", "ff1:Q", "out"};
  require(order.size() == database.get_timing_point_map().size(), "synthetic timing order does not cover every point");
  for (const std::string& name : order)
    require(database.get_timing_point_map().contains(name), "synthetic timing order names a missing point");
  for (const auto& arc : database.get_arc_list()) {
    const auto source = std::find(order.begin(), order.end(), arc.get_source_pin());
    const auto sink = std::find(order.begin(), order.end(), arc.get_sink_pin());
    require(source != order.end() && sink != order.end() && source < sink, "synthetic timing order violates native topology");
  }
}
}  // namespace

int main()
{
  try {
    constexpr std::uint64_t kToken = 53;
    auto empty_fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kEmpty);
    installSyntheticTimingOrder(empty_fixture.database());
    require(empty_fixture.database().get_timing_order_list().empty(), "empty graph timing order changed");
    auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
    installSyntheticTimingOrder(fixture.database());
    const CsrReferenceInput pristine = exportInput(fixture.database(), kToken);
    const CsrReferenceInputRawProjection raw_before = projectCsrReferenceInputRaw(fixture.database());
    const std::vector<std::uint8_t> canonical_before = canonicalReferenceInputBytes(pristine);
    const std::vector<std::uint8_t> raw_bytes_before = canonicalIndependentReferenceInputRawBytes(raw_before);
    const ReferencePoisonBinding binding{
        17,
        "00112233445566778899aabbccddeeff",
        kToken,
    };

    const std::vector<ReferencePoisonCanary> canaries = poisonReferenceDerivedState(fixture.database(), raw_before, binding);
    verifyCanaries(canaries, raw_before, binding);

    const CsrReferenceInput post = exportInput(fixture.database(), kToken);
    const CsrReferenceInputRawProjection raw_after = projectCsrReferenceInputRaw(fixture.database());
    require(canonicalReferenceInputBytes(post) == canonical_before, "derived poison changed production canonical candidate bytes");
    require(raw_after == raw_before && canonicalIndependentReferenceInputRawBytes(raw_after) == raw_bytes_before,
            "derived poison changed raw input projection");
    require(post.source_identity == pristine.source_identity, "derived poison changed source identity");
    const std::array hashes
        = {post.source_identity.topology_fingerprint, post.source_identity.clock_constraint_fingerprint, post.source_identity.library_fingerprint,
           post.source_identity.corner_fingerprint,   post.source_identity.parasitic_fingerprint,        post.source_identity.timing_input_fingerprint};
    for (const auto& hash : hashes)
      require(isLowercaseSha256(hash), "derived poison changed a component hash away from SHA-256");
  } catch (const std::exception& error) {
    std::cerr << "CsrReferencePoisonIsolationTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
