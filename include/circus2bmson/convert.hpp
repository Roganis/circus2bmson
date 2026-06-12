#ifndef CIRCUS2BMSON_CONVERT_HPP
#define CIRCUS2BMSON_CONVERT_HPP

#include <cstddef>
#include <string>

#include "circus2bmson/render.hpp"

namespace circus2bmson {

struct ConvertOptions {
  int max_loops = 1;
  std::string output_dir = ".";
  bool render_audio = true;  // false -> skeleton only (fast, no keysounds)
  KeysoundNaming keysound_naming = KeysoundNaming::Channel;
  bool volume_ramping = false;  // true -> libopenmpt smoothing (more dupes)
  AudioFormat audio_format = AudioFormat::Wav;
  // For MIDI input (future): user-supplied SoundFont replacing the bundled
  // default. Ignored by the tracker backends.
  std::string soundfont_path = "";
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

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_CONVERT_HPP
