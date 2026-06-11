#ifndef CIRCUS2BMSON_BMSON_HPP
#define CIRCUS2BMSON_BMSON_HPP

#include <string>
#include <vector>

#include "circus2bmson/score.hpp"

namespace circus2bmson {

// One bmson sound_channel: a keysound file and the pulses it plays at (BGM).
struct SoundChannel {
  std::string name;
  std::vector<long> note_pulses;
};

// Emit a bmson document (pretty-printed JSON). Notes sit on the BGM lane (x:0);
// info, bpm_events and lines come from the score.
std::string build_bmson(const Score& score,
                        const std::vector<SoundChannel>& channels);

// Skeleton without rendered audio: keysounds are provisional placeholders
// grouped by (instrument, note). ext is ".wav" or ".ogg".
std::string build_bmson_skeleton(const Score& score, const std::string& ext);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_BMSON_HPP
