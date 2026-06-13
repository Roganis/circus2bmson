#ifndef CIRCUS2BMSON_CHIPCONV_HPP
#define CIRCUS2BMSON_CHIPCONV_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "circus2bmson/convert.hpp"

// Chip-music -> bmson conversion (game-music-emu backend), mirroring the
// internal midiconv.hpp seam. Declared here so convert.cpp can dispatch to it;
// implemented in chip.cpp and only functional when libgme is present.
namespace circus2bmson {

// True if `ext` (with or without a leading dot, any case) is a chip format the
// libgme backend handles. Always false when built without libgme.
bool chip_handles_extension(const std::string& ext);

// Render each chip voice, detect note onsets, slice/dedup keysounds, and emit a
// bmson with an inferred constant BPM (note timing stays exact regardless).
// Throws std::runtime_error if libgme is unavailable.
ConvertResult convert_chip(const std::vector<std::uint8_t>& bytes,
                           const std::string& input_path,
                           const ConvertOptions& opts);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_CHIPCONV_HPP
