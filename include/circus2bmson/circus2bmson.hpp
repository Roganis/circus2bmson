#ifndef CIRCUS2BMSON_CIRCUS2BMSON_HPP
#define CIRCUS2BMSON_CIRCUS2BMSON_HPP

#include <string>

// Public surface of the circus2bmson core library.
//
// The conversion pipeline (MOD -> bmson) is implemented across milestones:
//   M1  build the musical timeline (note grid, bpm_events, lines)
//   M2  render per-channel stems, slice into deduplicated keysounds
//   M3  emit the bmson document + WAV folder
// Until then this header only exposes the library version.
namespace circus2bmson {

// Semantic version of the library/CLI, e.g. "0.0.1".
std::string version();

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_CIRCUS2BMSON_HPP
