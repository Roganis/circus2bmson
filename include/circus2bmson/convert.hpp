#ifndef CIRCUS2BMSON_CONVERT_HPP
#define CIRCUS2BMSON_CONVERT_HPP

#include <cstddef>
#include <functional>
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
  // For .dmf/.fur input: path to the Furnace binary that renders the chip
  // channels. Empty -> $C2B_FURNACE, else "furnace" on PATH. Ignored by the
  // other backends.
  std::string furnace_path = "";
  // Collapse keysounds that are not bit-identical but sound the same, given as
  // dB below the signal: 40 merges slices whose difference is at most -40 dB
  // (inaudible), 30 is more aggressive. 0 (the default) merges only byte-exact
  // duplicates.
  //
  // Chip stems repeat a drum hit or a held note with a slightly different phase
  // or envelope tail, so identical-sounding notes are rarely identical bytes --
  // on a Genesis module, -40 dB removes ~13% of the keysounds and -30 dB ~24%.
  // The cost is that consecutive keysounds are meant to butt together
  // seamlessly, and substituting a near-match can put a small discontinuity at
  // the seam; the looser the tolerance, the likelier that is to be audible.
  //
  // Only the stem backends (chip / .dmf / .fur) honour this.
  double dedup_tolerance_db = 0.0;

  // EXPERIMENTAL. Match keysounds on their magnitude spectrum, ignoring phase.
  //
  // Chip hardware re-renders "the same" sound differently every time -- a
  // Genesis DAC drum has 61 distinct (note, volume) pairs but 491 distinct
  // waveforms, because its resampling phase lands differently on each hit. Those
  // are identical to the ear and unmergeable by any sample-wise comparison, so
  // this ignores phase entirely and collapses them: ~5 500 keysounds becomes
  // ~1 700 on a dense Genesis module.
  //
  // The price is that slices are fragments of a continuously sounding channel,
  // not self-contained samples, so a phase-shifted substitute no longer joins
  // its neighbours smoothly. To stop that from clicking, every keysound is
  // faded at both edges (see keysound_fade_ms), which makes each one start and
  // end at zero -- so every seam is zero-to-zero and no substitution can step.
  // That fade is itself audible as a slight dip at every note boundary.
  //
  // Judge it by ear: measured against the original render it scores far worse
  // (-6 dB vs -47 dB) purely because phase differs, which the ear largely does
  // not hear. Off by default.
  bool dedup_ignore_phase = false;

  // Fade-out at the end of each keysound, in milliseconds. This is the edge that
  // keeps a seam quiet. Defaults to 0 (none), or 2 ms when dedup_ignore_phase is
  // on. <0 -> pick the default for the mode.
  double keysound_fade_ms = -1.0;

  // Fade-in at the start of each keysound, in milliseconds. Kept separate from
  // the fade-out and much shorter, because every millisecond of it blunts a
  // percussive attack: it only has to reach zero at the first sample, not
  // disguise anything. Defaults to 0.3 ms with dedup_ignore_phase, which is
  // ~13 samples -- enough to ramp instead of step.
  //
  // 0 means no fade-in at all: the sharpest possible attack, at the price of an
  // occasional pop where a substituted keysound starts on a non-zero sample.
  // <0 -> pick the default for the mode.
  double keysound_attack_ms = -1.0;
  // Gain applied to every keysound, in dB. The converted chart plays back at the
  // level the chip actually produced, which is a lot quieter than a mastered BMS
  // -- a Genesis mix peaks near full scale but has a ~16 dB crest factor, where
  // a commercial chart is limited to 8-10 dB. Matching that properly needs a
  // limiter across the *mix*, which we cannot do: the player sums the keysounds,
  // and a time-varying gain would make every repeat of a note unique and undo
  // the deduplication entirely.
  //
  // So instead: turn it up, and accept that the rare peak clips. See auto_gain.
  double gain_db = 0.0;

  // Pick gain_db automatically: the loudest setting at which no more than
  // clip_budget of the summed mix's samples would exceed full scale. Peaks in
  // chip music are short and sparse, so a small budget buys real loudness --
  // 0.1% is worth ~5 dB on a Genesis module, against the 0.8 dB that clipping
  // nothing at all would allow. Overrides gain_db.
  //
  // On by default: left alone, a chip chart is ~5 dB quieter than everything
  // else in a player's library, which is worse than a peak clipped once in a
  // thousand samples. Only the stem backends (chip / .dmf / .fur) use this --
  // the tracker and MIDI paths are untouched.
  bool auto_gain = true;
  double clip_budget = 0.001;  // fraction of samples allowed to clip (0.1%)

  // Optional: called as the conversion moves through its slow stages, so a UI
  // can say what it is doing -- a long chip conversion spends minutes rendering
  // and encoding, and a silent "converting..." is indistinguishable from a
  // hang. Invoked from the converting thread (and, during the Furnace render,
  // from a watcher thread), so it must tolerate being called off the UI thread.
  // ProgressFn comes from render.hpp. May be null.
  ProgressFn on_progress;
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
