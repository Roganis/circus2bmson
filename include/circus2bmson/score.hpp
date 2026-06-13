#ifndef CIRCUS2BMSON_SCORE_HPP
#define CIRCUS2BMSON_SCORE_HPP

#include <cstdint>
#include <string>
#include <vector>

// Format-agnostic score model, built by playing the module once through
// libopenmpt. This replaces format-specific parsing/sequencing for conversion:
// control flow (jumps, breaks, loops, pattern delay) and tempo semantics
// (speed/tempo, S3M/IT tempo modes, slides) are whatever libopenmpt actually
// played. Works for every format libopenmpt supports (MOD and variants, XM,
// S3M, IT, ...).
//
// Timing model: each played row spans 60 pulses (resolution 240, the usual
// 4-rows-per-beat tracker convention). Row durations are measured from the
// audio stream itself, so bmson BPM events keep the pulse grid exactly aligned
// with the rendered keysounds: nominal bpm = 6 * tempo / speed when the
// measured duration agrees with it, otherwise the measured per-row bpm
// (15 / row_seconds) -- which transparently covers pattern delay and tempo
// slides. The note-delay effect (EDx on MOD/XM, SDx on S3M/IT) places a note
// part-way into its row, both in pulses and in its slice onset frame.
namespace circus2bmson {

struct PlayedRow {
  int order = 0;
  int row = 0;      // row within the pattern
  int pattern = 0;
  long frame = 0;   // onset in the rendered stream (at Score::sample_rate)
};

struct ScoreNote {
  int row_index = 0;  // index into Score::rows
  int channel = 0;
  int note = 0;        // libopenmpt note value (1..120; 1 = C-0)
  int instrument = 0;  // effective instrument/sample number (latched), 0 if none
  long pulse = 0;
  long frame = 0;      // onset frame in the rendered stream (sub-row aware)
};

struct BpmEvent {
  long pulse = 0;
  double bpm = 0.0;
};

struct Score {
  std::string title;
  std::string format;          // libopenmpt type, e.g. "mod", "xm", "s3m", "it"
  int channels = 0;
  int sample_rate = 44100;
  int repeat_count = 0;        // value passed to libopenmpt (max_loops - 1)
  bool use_instruments = false;          // instruments (XM/IT) vs samples
  std::vector<std::string> key_names;    // instrument or sample names (1-based)

  int resolution = 240;
  int pulses_per_row = 60;
  double init_bpm = 125.0;
  long total_pulses = 0;
  long total_frames = 0;       // length of one full render
  double total_seconds = 0.0;

  std::vector<PlayedRow> rows;     // in play order
  std::vector<ScoreNote> notes;    // note-ons in play order
  std::vector<BpmEvent> bpm_events;
  std::vector<long> lines;         // bar-line pulses

  // Diagnostics.
  bool truncated = false;   // hit the safety length cap
  int coarse_rows = 0;      // rows whose onset could not be pinned exactly
  int backward_jumps = 0;   // backward order transitions played (loops/jumps)
};

// Play the module once (unrolling loops max_loops times) and extract the score.
// Throws std::runtime_error on unsupported/corrupt input.
Score read_score(const std::vector<std::uint8_t>& bytes, int max_loops = 1);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_SCORE_HPP
