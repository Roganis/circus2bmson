#ifndef CIRCUS2BMSON_BMSON_HPP
#define CIRCUS2BMSON_BMSON_HPP

#include <string>
#include <vector>

#include "circus2bmson/mod.hpp"
#include "circus2bmson/timeline.hpp"

namespace circus2bmson {

// One bmson sound_channel: a keysound file and the pulses it plays at (BGM).
struct SoundChannel {
  std::string name;
  std::vector<long> note_pulses;
};

// Emit a bmson document (pretty-printed JSON). Notes sit on the BGM lane (x:0);
// info, bpm_events and lines come from the timeline.
std::string build_bmson(const Module& mod, const Timeline& tl,
                        const std::vector<SoundChannel>& channels);

// M1 skeleton: keysounds are provisional, grouped by (sample, period), pending
// the real audio rendered in M2.
std::string build_bmson_skeleton(const Module& mod, const Timeline& tl);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_BMSON_HPP
