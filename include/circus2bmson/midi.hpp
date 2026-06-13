#ifndef CIRCUS2BMSON_MIDI_HPP
#define CIRCUS2BMSON_MIDI_HPP

#include <cstdint>
#include <string>
#include <vector>

// Minimal Standard MIDI File (SMF) reader. Produces explicit note events with
// durations, the tempo map and bar lines -- everything the bmson timeline and
// the per-note synth renderer need. Unlike trackers, MIDI is polyphonic, so
// notes carry their own on/off ticks rather than being sliced between onsets.
namespace circus2bmson {

struct MidiNote {
  long tick_on = 0;
  long tick_off = 0;
  int channel = 0;     // 0..15
  int key = 0;         // 0..127
  int velocity = 1;    // 1..127
  int program = 0;     // GM program active on the channel at tick_on
  bool drum = false;   // channel 9 (GM percussion)
  int volume = 100;    // CC7  channel volume at tick_on (0..127, GM default 100)
  int expression = 127;  // CC11 expression at tick_on (0..127, GM default 127)
  int pan = 64;        // CC10 pan at tick_on (0..127, 64 = centre)
};

struct MidiTempo {
  long tick = 0;
  int usec_per_qn = 500000;  // microseconds per quarter note (120 BPM default)
};

struct MidiSong {
  int division = 480;  // ticks per quarter note (PPQN)
  std::string title;
  std::vector<MidiNote> notes;    // sorted by tick_on
  std::vector<MidiTempo> tempos;  // sorted by tick (always has one at tick 0)
  std::vector<long> bar_ticks;    // bar-line ticks
  long end_tick = 0;
};

// Parse a Standard MIDI File (format 0 or 1). Throws std::runtime_error on
// malformed input or unsupported (SMPTE) timing.
MidiSong parse_smf(const std::vector<std::uint8_t>& bytes);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_MIDI_HPP
