#include "circus2bmson/onset.hpp"

#include <algorithm>
#include <cmath>

namespace circus2bmson {

std::vector<long> detect_onsets(const float* audio, long frames,
                                int sample_rate, const OnsetParams& p) {
  std::vector<long> onsets;
  if (!audio || frames <= 0 || p.hop < 1) return onsets;

  const long min_gap =
      std::max<long>(1, static_cast<long>(p.min_gap_seconds * sample_rate));
  bool active = false;       // currently inside a sounding note
  long last_onset = -min_gap;  // so an attack at frame 0 still counts

  for (long start = 0; start < frames; start += p.hop) {
    const long end = std::min(frames, start + p.hop);
    float peak = 0.0f;  // window peak across both channels
    for (long f = start; f < end; ++f) {
      peak = std::max(peak, std::fabs(audio[2 * f]));
      peak = std::max(peak, std::fabs(audio[2 * f + 1]));
    }
    if (!active && peak >= p.on_threshold) {
      active = true;
      if (start - last_onset >= min_gap) {
        onsets.push_back(start);
        last_onset = start;
      }
    } else if (active && peak < p.off_threshold) {
      active = false;  // re-arm for the next attack
    }
  }
  return onsets;
}

}  // namespace circus2bmson
