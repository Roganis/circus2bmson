#include "circus2bmson/mod.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace circus2bmson {
namespace {

std::uint16_t be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// Trim trailing NULs/spaces from a fixed-width MOD string field.
std::string trim_field(const std::uint8_t* p, std::size_t n) {
  std::size_t end = 0;
  for (std::size_t i = 0; i < n; ++i)
    if (p[i] != 0) end = i + 1;
  std::string s(reinterpret_cast<const char*>(p), end);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}

// Map a 4-char signature to a channel count. Returns 0 if unsupported.
int channels_for_tag(const std::string& tag) {
  if (tag == "M.K." || tag == "M!K!" || tag == "M&K!" || tag == "N.T." ||
      tag == "FLT4" || tag == "4CHN")
    return 4;
  if (tag == "6CHN") return 6;
  if (tag == "8CHN") return 8;
  // "nCHN" / "nnCH" style tags.
  auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  if (is_digit(tag[0]) && tag[1] == 'C' && tag[2] == 'H' && tag[3] == 'N')
    return tag[0] - '0';
  if (is_digit(tag[0]) && is_digit(tag[1]) && tag[2] == 'C' && tag[3] == 'H')
    return (tag[0] - '0') * 10 + (tag[1] - '0');
  return 0;
}

}  // namespace

Module parse_mod(const std::vector<std::uint8_t>& b) {
  constexpr std::size_t kNumSamples = 31;
  constexpr std::size_t kSampleHdr = 30;
  constexpr std::size_t kOrderBase = 20 + kNumSamples * kSampleHdr;  // 950
  constexpr std::size_t kPatternBase = 1084;
  constexpr int kRowsPerPattern = 64;

  if (b.size() < kPatternBase)
    throw std::runtime_error("file too small to be a 31-sample MOD");

  Module m;
  m.title = trim_field(b.data(), 20);
  m.format_tag = std::string(reinterpret_cast<const char*>(b.data() + 1080), 4);
  m.channels = channels_for_tag(m.format_tag);
  if (m.channels == 0)
    throw std::runtime_error("unsupported MOD signature '" + m.format_tag +
                             "' (only 31-sample MODs are supported)");

  // Sample headers.
  m.samples.resize(kNumSamples);
  for (std::size_t i = 0; i < kNumSamples; ++i) {
    const std::uint8_t* p = b.data() + 20 + i * kSampleHdr;
    ModSample& s = m.samples[i];
    s.name = trim_field(p, 22);
    s.length = static_cast<std::uint32_t>(be16(p + 22)) * 2;
    s.finetune = static_cast<std::int8_t>((p[24] & 0x0F) < 8 ? (p[24] & 0x0F)
                                                             : (p[24] & 0x0F) - 16);
    s.volume = p[25];
    s.loop_start = static_cast<std::uint32_t>(be16(p + 26)) * 2;
    s.loop_length = static_cast<std::uint32_t>(be16(p + 28)) * 2;
  }

  m.song_length = b[kOrderBase];
  m.restart = b[kOrderBase + 1];
  if (m.song_length < 1 || m.song_length > 128)
    throw std::runtime_error("invalid song length " + std::to_string(m.song_length));

  m.order.assign(b.begin() + kOrderBase + 2, b.begin() + kOrderBase + 2 + 128);

  // Number of patterns = highest pattern index referenced anywhere + 1.
  int num_patterns = 0;
  for (std::uint8_t o : m.order) num_patterns = std::max(num_patterns, o + 1);

  const std::size_t cell_count =
      static_cast<std::size_t>(kRowsPerPattern) * m.channels;
  const std::size_t pattern_bytes = cell_count * 4;
  const std::size_t needed = kPatternBase + pattern_bytes * num_patterns;
  if (b.size() < needed)
    throw std::runtime_error("truncated pattern data (need " +
                             std::to_string(needed) + " bytes, have " +
                             std::to_string(b.size()) + ")");

  m.patterns.resize(num_patterns);
  for (int p = 0; p < num_patterns; ++p) {
    ModPattern& pat = m.patterns[p];
    pat.rows = kRowsPerPattern;
    pat.channels = m.channels;
    pat.cells.resize(cell_count);
    const std::uint8_t* base = b.data() + kPatternBase + pattern_bytes * p;
    for (std::size_t i = 0; i < cell_count; ++i) {
      const std::uint8_t* c = base + i * 4;
      ModCell& cell = pat.cells[i];
      cell.sample = static_cast<std::uint8_t>((c[0] & 0xF0) | (c[2] >> 4));
      cell.period = static_cast<std::uint16_t>(((c[0] & 0x0F) << 8) | c[1]);
      cell.effect = static_cast<std::uint8_t>(c[2] & 0x0F);
      cell.param = c[3];
    }
  }

  return m;
}

}  // namespace circus2bmson
