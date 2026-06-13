#ifndef CIRCUS2BMSON_MIDICONV_HPP
#define CIRCUS2BMSON_MIDICONV_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "circus2bmson/convert.hpp"

namespace circus2bmson {

// Convert a Standard MIDI File to a bmson folder: parse notes, render each one
// in isolation through a SoundFont (FluidSynth), deduplicate and write the
// keysounds, and emit the bmson. Called by convert_mod_file for .mid/.midi.
ConvertResult convert_midi(const std::vector<std::uint8_t>& bytes,
                           const std::string& input_path,
                           const ConvertOptions& opts);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_MIDICONV_HPP
