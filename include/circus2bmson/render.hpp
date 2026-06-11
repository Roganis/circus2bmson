#ifndef CIRCUS2BMSON_RENDER_HPP
#define CIRCUS2BMSON_RENDER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "circus2bmson/mod.hpp"

namespace circus2bmson {

// How keysound WAV files are named, so they cluster in an editor's sound list.
//   Instrument: s05_bass_ch01_A-2.wav   (group by sample/instrument)
//   Lane:       ch01_s05_bass_A-2.wav   (group by MOD channel)
enum class KeysoundNaming { Instrument, Lane };

// Stable identity for a note-on cell, used to bind a timeline note to its
// rendered keysound. Order < 128, row < 64, channel < 64.
inline std::uint32_t note_key(int order, int row, int channel) {
  return ((static_cast<std::uint32_t>(order) * 64u + row) * 64u + channel);
}

struct RenderResult {
  int sample_rate = 44100;
  std::vector<std::string> keysound_names;  // id -> WAV filename (relative)
  std::unordered_map<std::uint32_t, int> note_to_keysound;  // note_key -> id

  long render_frames = 0;   // length of one playthrough, in frames
  long marker_rows = 0;     // rows libopenmpt played (for alignment checks)
  long total_slices = 0;    // note-ons sliced before dedup
  long unique_keysounds = 0;
};

// Render each channel in isolation via libopenmpt, slice every stem at its
// note-on times, deduplicate identical audio, and write the unique keysounds as
// stereo 16-bit WAVs into out_dir. Effects, pitch and panning are baked in.
RenderResult render_keysounds(const std::vector<std::uint8_t>& bytes,
                              const Module& mod, const std::string& out_dir,
                              KeysoundNaming naming = KeysoundNaming::Instrument);

// End-to-end fidelity check: slice the module into keysounds, place each note's
// keysound back at its onset, and compare the reconstruction to libopenmpt's
// full mix. Returns the residual RMS relative to the signal, in dB (more
// negative is better). A clean pipeline yields a very low residual.
double reconstruct_residual_db(const std::vector<std::uint8_t>& bytes,
                               const Module& mod);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_RENDER_HPP
