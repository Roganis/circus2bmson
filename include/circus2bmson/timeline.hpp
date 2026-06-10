#ifndef CIRCUS2BMSON_TIMELINE_HPP
#define CIRCUS2BMSON_TIMELINE_HPP

#include <cstdint>
#include <vector>

#include "circus2bmson/mod.hpp"

// Build a flattened musical timeline from a Module: unroll the order list,
// follow position jumps / pattern breaks / pattern loops, and place every
// note-on on a clean pulse grid (one row = resolution/4 pulses). Speed and
// tempo are folded into bmson tempo as bpm = 6 * tempo / speed.
namespace circus2bmson {

// A note-on, positioned in pulses on the BGM lane.
struct NoteEvent {
  int order = 0;
  int row = 0;
  int channel = 0;
  std::uint8_t sample = 0;   // effective sample (1..31)
  std::uint16_t period = 0;  // Amiga period
  long pulse = 0;
};

struct BpmEvent {
  long pulse = 0;
  double bpm = 0.0;
};

struct TimelineOptions {
  // Maximum number of times any song position may play. With the default the
  // song plays through once and stops at the first backward loop; raise it to
  // unroll a looping section multiple times. Pattern loops (E6x) are bounded
  // independently and not limited by this.
  int max_loops = 1;
};

struct Timeline {
  int resolution = 240;          // pulses per quarter note
  int pulses_per_row = 60;       // resolution / 4
  double init_bpm = 125.0;
  long total_pulses = 0;

  std::vector<NoteEvent> notes;
  std::vector<BpmEvent> bpm_events;
  std::vector<long> lines;       // bar-line pulses

  // Diagnostics.
  long emitted_rows = 0;
  double total_seconds = 0.0;    // real-time length implied by speed/tempo
  int loops_played = 0;          // backward-loop iterations unrolled
  bool truncated = false;        // hit the safety row cap
  int unsupported_flow = 0;      // count of control-flow effects not modelled
};

Timeline build_timeline(const Module& mod, const TimelineOptions& opts = {});

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_TIMELINE_HPP
