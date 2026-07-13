#ifndef CIRCUS2BMSON_RENDER_HPP
#define CIRCUS2BMSON_RENDER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "circus2bmson/score.hpp"

namespace circus2bmson {

// How keysound files are named, so they cluster in an editor's sound list.
//   Channel:    channel1_001.wav       (default; simple, grouped by channel)
//   Instrument: s05_bass_ch01_A-2.wav  (descriptive, grouped by instrument)
//   Lane:       ch01_s05_bass_A-2.wav  (descriptive, grouped by channel)
enum class KeysoundNaming { Channel, Instrument, Lane };

// Keysound audio container.
enum class AudioFormat { Wav, Ogg };

inline const char* audio_extension(AudioFormat f) {
  return f == AudioFormat::Ogg ? ".ogg" : ".wav";
}

struct RenderResult {
  int sample_rate = 44100;
  std::vector<std::string> keysound_names;  // id -> filename (relative)
  std::vector<int> note_keysound;           // per Score::notes index -> id

  long total_slices = 0;     // note-ons sliced before dedup
  long unique_keysounds = 0;
};

// Reports progress as each channel is rendered; may be null. Declared here
// rather than taken from convert.hpp to keep this header free of that include.
using ProgressFn = std::function<void(const std::string& message)>;

// Render each pattern channel in isolation via libopenmpt, slice every stem at
// the score's note-on frames, deduplicate identical audio, and write the unique
// keysounds into out_dir. Pitch, effects and panning are baked in.
RenderResult render_keysounds(const std::vector<std::uint8_t>& bytes,
                              const Score& score, const std::string& out_dir,
                              KeysoundNaming naming = KeysoundNaming::Channel,
                              bool volume_ramping = false,
                              AudioFormat format = AudioFormat::Wav,
                              const ProgressFn& progress = nullptr);

// End-to-end fidelity check: slice the module into keysounds, place each note's
// keysound back at its onset, and compare the reconstruction to libopenmpt's
// full mix. Returns the residual RMS relative to the signal, in dB (more
// negative is better).
double reconstruct_residual_db(const std::vector<std::uint8_t>& bytes,
                               const Score& score,
                               bool volume_ramping = false);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_RENDER_HPP
