#ifndef CIRCUS2BMSON_CONVERT_HPP
#define CIRCUS2BMSON_CONVERT_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "circus2bmson/midimix.hpp"
#include "circus2bmson/render.hpp"

namespace circus2bmson {

struct ConvertOptions {
  int max_loops = 1;
  std::string output_dir = ".";
  bool render_audio = true;  // false -> skeleton only (fast, no keysounds)
  KeysoundNaming keysound_naming = KeysoundNaming::Channel;
  bool volume_ramping = false;  // true -> libopenmpt smoothing (more dupes)
  AudioFormat audio_format = AudioFormat::Wav;
  // For MIDI input: user-supplied SoundFont replacing the bundled default.
  // Ignored by the tracker backends.
  std::string soundfont_path = "";
  // For MIDI input: per-channel / per-instrument gains and whether to honour
  // the file's own volume/expression controllers. Baked into the keysounds.
  // Ignored by the tracker backends.
  MidiMix midi_mix;
};

struct ConvertResult {
  std::string bmson_path;
  std::string title;
  std::string format;  // module type as reported by libopenmpt
  int channels = 0;
  std::size_t note_count = 0;
  std::size_t bpm_event_count = 0;
  std::size_t line_count = 0;
  double init_bpm = 0.0;
  double total_seconds = 0.0;
  long total_pulses = 0;
  long emitted_rows = 0;
  bool truncated = false;
  int coarse_rows = 0;

  // Audio (when render_audio is true).
  bool audio_rendered = false;
  long total_slices = 0;    // note-ons sliced before dedup
  long keysound_count = 0;  // unique keysound files written
};

// Convert a tracker module (any format libopenmpt supports) to a bmson folder.
// Writes <output_dir>/<input-stem>.bmson plus keysound files and returns stats.
// Throws std::exception on I/O or parse failure.
ConvertResult convert_mod_file(const std::string& input_path,
                               const ConvertOptions& opts);

// Resolve a SoundFont path for MIDI rendering: the given path if non-empty
// (must exist), else $C2B_SOUNDFONT, the bundled FluidR3_GM.sf2, or a common
// system location. Throws std::runtime_error if none is found.
std::string resolve_soundfont(const std::string& given);

// Input file extensions this build accepts (lower-case, no leading dot, sorted):
// every tracker format the linked libopenmpt supports, plus "mid"/"midi" for
// the MIDI backend. Sourced from libopenmpt so it never drifts from reality;
// handy for file-dialog filters and CLI help.
std::vector<std::string> supported_input_extensions();

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_CONVERT_HPP
