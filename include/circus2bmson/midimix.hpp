#ifndef CIRCUS2BMSON_MIDIMIX_HPP
#define CIRCUS2BMSON_MIDIMIX_HPP

#include <array>
#include <map>

// A user-authored mix for MIDI input: a linear gain per MIDI channel and per
// instrument, layered on top of the file's own volume/expression controllers.
// Shared by the offline converter (baked into the rendered keysounds) and the
// live MidiPlayer preview, so what you hear is what you get.
namespace circus2bmson {

// Drums (GM channel 10 / bank 128) group under this instrument index, distinct
// from melodic programs 0..127.
constexpr int kDrumProgram = 128;

struct MidiMix {
  std::array<float, 16> channel_gain;  // per MIDI channel 0..15, linear
  std::map<int, float> program_gain;   // program (0..127, kDrumProgram) -> gain
  bool honor_cc = true;                // apply the file's CC7/CC11/CC10

  MidiMix() { channel_gain.fill(1.0f); }

  float channel(int ch) const {
    return (ch >= 0 && ch < 16) ? channel_gain[ch] : 1.0f;
  }
  float program(int prog) const {
    const auto it = program_gain.find(prog);
    return it == program_gain.end() ? 1.0f : it->second;
  }
  // Combined linear gain for a note on `ch` playing instrument `prog`
  // (kDrumProgram for percussion).
  float user_gain(int ch, int prog) const {
    return channel(ch) * program(prog);
  }
};

// Instrument index a note belongs to: its GM program, or kDrumProgram if it is
// a percussion note.
inline int instrument_of(int program, bool drum) {
  return drum ? kDrumProgram : program;
}

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_MIDIMIX_HPP
