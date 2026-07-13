#ifndef CIRCUS2BMSON_STEMCONV_HPP
#define CIRCUS2BMSON_STEMCONV_HPP

#include <string>
#include <vector>

#include "circus2bmson/convert.hpp"

// The shared back half of every "audio-domain" backend: a song that has already
// been rendered to one isolated stem per voice, but carries no note events. The
// notes have to be recovered from the audio (onset.hpp) and the grid's BPM
// guessed, so this is the lower-fidelity tier -- unlike the libopenmpt and MIDI
// backends, which know where the notes are.
//
// Two backends land in this shape: game-music-emu voices (chip.cpp) and Furnace
// channels (furnace.cpp). Everything downstream of "here are the stems" -- onset
// detection, slicing, dedup, keysound naming, the bmson emit -- is identical, so
// it lives here once.
namespace circus2bmson {

// Say what we are doing, if anyone is listening.
inline void report(const ConvertOptions& opts, const std::string& message) {
  if (opts.on_progress) opts.on_progress(message);
}

struct StemSong {
  std::string title;
  std::string format;                     // "nsf", "vgm", "dmf", "fur", ...
  int sample_rate = 44100;
  long total_frames = 0;                  // rendered length, in frames
  std::vector<std::string> voice_names;   // one per stem, for keysound names
  std::vector<std::vector<float>> stems;  // interleaved stereo, one per voice

  // Known note-on frames per voice, when the backend can supply them (the
  // Furnace backend reads them out of the renderer's command stream). Empty ->
  // fall back to detecting them from the audio, which cannot see a note that is
  // only a pitch change. Prefer these whenever they exist.
  std::vector<std::vector<long>> onsets;

  // The module's real tempo, when the backend can read it. 0 -> guess one from
  // the note spacing. Note timing is exact either way (pulses come from frames);
  // this decides where the bmson's gridlines and bars fall, which is what makes
  // the result editable in a chart editor.
  double bpm = 0.0;
};

// Detect each stem's note onsets, slice it at them, deduplicate identical PCM,
// and write <output_dir>/<input-stem>.bmson plus the keysound files.
//
// Note timing stays exact -- pulses are computed straight from frames -- so the
// inferred BPM only decides where the gridlines land, not where the notes do.
ConvertResult convert_stems(const StemSong& song, const std::string& input_path,
                            const ConvertOptions& opts);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_STEMCONV_HPP
