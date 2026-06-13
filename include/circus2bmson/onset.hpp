#ifndef CIRCUS2BMSON_ONSET_HPP
#define CIRCUS2BMSON_ONSET_HPP

#include <vector>

// Audio-domain note-onset detection. Chip formats (NSF, GBS, VGM, ...) expose
// no note events, so for the game-music-emu backend we derive a voice's note
// onsets from its rendered audio: a new attack is a rising edge where the
// envelope crosses from "quiet" up to "active". Hysteresis plus a refractory
// gap keep a single attack from registering twice. This pins notes separated by
// silence or a decay; truly legato lines (no amplitude dip between notes) are a
// known limitation handled by later refinements.
namespace circus2bmson {

struct OnsetParams {
  int hop = 128;               // analysis window, in frames
  float on_threshold = 0.02f;  // peak amplitude (0..1) to enter "active"
  float off_threshold = 0.008f;  // drop below this to re-arm (hysteresis)
  double min_gap_seconds = 0.025;  // refractory period between onsets
};

// Onset frames (start of each detected note) in interleaved stereo float audio,
// in increasing order. `frames` is the number of stereo frames in `audio`.
std::vector<long> detect_onsets(const float* audio, long frames, int sample_rate,
                                const OnsetParams& params = {});

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_ONSET_HPP
