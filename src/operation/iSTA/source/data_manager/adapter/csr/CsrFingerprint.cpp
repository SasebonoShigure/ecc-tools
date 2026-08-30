// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "CsrFingerprint.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "TimingSemanticKey.hpp"

namespace ista::csr {

namespace {

constexpr std::array<std::uint32_t, 64> kSha256Round
    = {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU,
       0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU,
       0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U,
       0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
       0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
       0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
  return (x & y) ^ (~x & z);
}

std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
  return (x & y) ^ (x & z) ^ (y & z);
}

std::uint32_t bigSigma0(std::uint32_t x)
{
  return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}

std::uint32_t bigSigma1(std::uint32_t x)
{
  return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}

std::uint32_t smallSigma0(std::uint32_t x)
{
  return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
}

std::uint32_t smallSigma1(std::uint32_t x)
{
  return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
}

void compressSha256(std::array<std::uint32_t, 8>& state, std::span<const std::uint8_t, 64> block)
{
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    std::size_t offset = index * 4;
    words[index] = (static_cast<std::uint32_t>(block[offset]) << 24) | (static_cast<std::uint32_t>(block[offset + 1]) << 16)
                   | (static_cast<std::uint32_t>(block[offset + 2]) << 8) | static_cast<std::uint32_t>(block[offset + 3]);
  }
  for (std::size_t index = 16; index < words.size(); ++index)
    words[index] = smallSigma1(words[index - 2]) + words[index - 7] + smallSigma0(words[index - 15]) + words[index - 16];

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    std::uint32_t temp1 = h + bigSigma1(e) + choose(e, f, g) + kSha256Round[index] + words[index];
    std::uint32_t temp2 = bigSigma0(a) + majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

std::string componentFingerprint(std::string_view domain, std::span<const std::uint8_t> payload)
{
  CanonicalEncoder encoder;
  encoder.appendDomain(domain);
  encoder.appendBytes(payload);
  return sha256Hex(encoder.bytes());
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> value)
{
  if (value.size() > (std::numeric_limits<std::uint64_t>::max() / 8))
    throw std::length_error("SHA-256 input is too large");
  std::vector<std::uint8_t> padded(value.begin(), value.end());
  std::uint64_t bit_length = static_cast<std::uint64_t>(value.size()) * 8;
  padded.push_back(0x80);
  while ((padded.size() % 64) != 56)
    padded.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8)
    padded.push_back(static_cast<std::uint8_t>(bit_length >> shift));

  std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t offset = 0; offset < padded.size(); offset += 64)
    compressSha256(state, std::span<const std::uint8_t, 64>(padded.data() + offset, 64));

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    digest[index * 4] = static_cast<std::uint8_t>(state[index] >> 24);
    digest[index * 4 + 1] = static_cast<std::uint8_t>(state[index] >> 16);
    digest[index * 4 + 2] = static_cast<std::uint8_t>(state[index] >> 8);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state[index]);
  }
  return digest;
}

std::string sha256Hex(std::span<const std::uint8_t> value)
{
  constexpr char kHex[] = "0123456789abcdef";
  const auto digest = sha256(value);
  std::string result(digest.size() * 2, '\0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    result[index * 2] = kHex[digest[index] >> 4];
    result[index * 2 + 1] = kHex[digest[index] & 0x0fU];
  }
  return result;
}

bool isLowercaseSha256(std::string_view value)
{
  return value.size() == 64 && std::ranges::all_of(value, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

CsrSnapshotIdentity makeSnapshotIdentity(std::uint64_t session_token, std::uint64_t graph_generation, const FingerprintPayloads& payloads)
{
  if (session_token == 0)
    throw std::invalid_argument("session token must be nonzero");
  CsrSnapshotIdentity identity;
  identity.session_token = session_token;
  identity.graph_generation = graph_generation;
  identity.topology_fingerprint = componentFingerprint("topology-fingerprint", payloads.topology);
  identity.clock_constraint_fingerprint = componentFingerprint("clock-constraint-fingerprint", payloads.clock_constraint);
  identity.library_fingerprint = componentFingerprint("library-fingerprint", payloads.library);
  identity.corner_fingerprint = componentFingerprint("corner-fingerprint", payloads.corner);
  identity.parasitic_fingerprint = componentFingerprint("parasitic-fingerprint", payloads.parasitic);

  CanonicalEncoder timing_input;
  timing_input.appendDomain("timing-input-composite");
  timing_input.appendString(identity.topology_fingerprint);
  timing_input.appendString(identity.clock_constraint_fingerprint);
  timing_input.appendString(identity.library_fingerprint);
  timing_input.appendString(identity.corner_fingerprint);
  timing_input.appendString(identity.parasitic_fingerprint);
  timing_input.appendBytes(payloads.timing_input);
  identity.timing_input_fingerprint = sha256Hex(timing_input.bytes());
  return identity;
}

}  // namespace ista::csr
