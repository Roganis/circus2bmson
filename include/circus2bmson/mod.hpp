#ifndef CIRCUS2BMSON_MOD_HPP
#define CIRCUS2BMSON_MOD_HPP

#include <cstdint>
#include <string>
#include <vector>

// Minimal ProTracker (.mod) parser: just the score (header, samples headers,
// order list and pattern cells). Sample PCM and audio rendering are handled by
// libopenmpt in M2; here we only need exact note/effect data to build the
// musical timeline.
namespace circus2bmson {

// One pattern cell, decoded from the raw 4-byte MOD encoding.
struct ModCell {
  std::uint8_t sample = 0;   // 1..31, or 0 for "none"
  std::uint16_t period = 0;  // Amiga period; 0 means "no note"
  std::uint8_t effect = 0;   // effect command nibble (0x0..0xF)
  std::uint8_t param = 0;    // effect parameter byte
};

// Sample header metadata (PCM data itself is not loaded here).
struct ModSample {
  std::string name;
  std::uint32_t length = 0;        // bytes
  std::int8_t finetune = 0;        // -8..7
  std::uint8_t volume = 0;         // 0..64
  std::uint32_t loop_start = 0;    // bytes
  std::uint32_t loop_length = 0;   // bytes
  bool looped() const { return loop_length > 2; }
};

struct ModPattern {
  int rows = 64;
  int channels = 4;
  std::vector<ModCell> cells;  // row-major: cells[row * channels + channel]
  const ModCell& at(int row, int channel) const {
    return cells[static_cast<std::size_t>(row) * channels + channel];
  }
};

struct Module {
  std::string title;
  std::string format_tag;            // 4-char magic, e.g. "M.K."
  int channels = 4;
  int song_length = 0;               // number of used order entries
  std::uint8_t restart = 0;          // restart position byte (informational)
  std::vector<std::uint8_t> order;   // 128 entries; first song_length are used
  std::vector<ModSample> samples;    // 31 entries (index 0 == sample 1)
  std::vector<ModPattern> patterns;
};

// Parse a 31-sample MOD with a recognised channel signature.
// Throws std::runtime_error on unsupported/corrupt input.
Module parse_mod(const std::vector<std::uint8_t>& bytes);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_MOD_HPP
