#ifndef CIRCUS2BMSON_BMSON_HPP
#define CIRCUS2BMSON_BMSON_HPP

#include <string>

#include "circus2bmson/mod.hpp"
#include "circus2bmson/timeline.hpp"

namespace circus2bmson {

// Build a bmson document as pretty-printed JSON.
//
// M1 emits a *skeleton*: info header, bpm_events and lines are final, but every
// note sits on the BGM lane (x:0) referencing a provisional keysound name
// grouped by (sample, period). M2 replaces those with rendered-audio keysounds.
std::string build_bmson_skeleton(const Module& mod, const Timeline& tl);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_BMSON_HPP
