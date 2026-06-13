#include "midisynth.hpp"

#include <algorithm>
#include <cmath>

#include <fluidsynth.h>

namespace circus2bmson {
namespace {
constexpr int kBlock = 1024;
constexpr float kSilence = 1.0e-4f;

// No-op log sink used to swallow FluidSynth's audio-driver probe warnings
// during setup (we render offline and never open an audio driver).
void silent_log(int /*level*/, const char* /*message*/, void* /*data*/) {}
}  // namespace

MidiSynth::MidiSynth(const std::string& soundfont_path, int sample_rate)
    : rate_(sample_rate) {
  // Creating settings makes FluidSynth enumerate every compiled-in audio
  // driver; the SDL2/SDL3 driver warns when SDL audio isn't initialized. We
  // render offline and never open an audio driver, so mute warnings during
  // setup, then restore the previous handler so real diagnostics (e.g. a
  // SoundFont that fails to load) still reach the console.
  fluid_log_function_t prev_warn =
      fluid_set_log_function(FLUID_WARN, silent_log, nullptr);
  fluid_settings_t* settings = new_fluid_settings();
  settings_ = settings;
  fluid_settings_setnum(settings, "synth.sample-rate",
                        static_cast<double>(sample_rate));
  fluid_settings_setint(settings, "synth.reverb.active", 0);
  fluid_settings_setint(settings, "synth.chorus.active", 0);
  fluid_settings_setnum(settings, "synth.gain", 0.6);
  fluid_settings_setint(settings, "synth.polyphony", 64);
  fluid_synth_t* synth = new_fluid_synth(settings);
  synth_ = synth;
  fluid_set_log_function(FLUID_WARN, prev_warn, nullptr);
  if (synth) {
    sfid_ = fluid_synth_sfload(synth, soundfont_path.c_str(), 1);
  }
}

MidiSynth::~MidiSynth() {
  if (synth_) delete_fluid_synth(static_cast<fluid_synth_t*>(synth_));
  if (settings_) delete_fluid_settings(static_cast<fluid_settings_t*>(settings_));
}

std::vector<float> MidiSynth::render_note(int program, int key, int velocity,
                                          bool drum, long duration_frames,
                                          int volume, int expression, int pan,
                                          float gain) {
  auto* synth = static_cast<fluid_synth_t*>(synth_);
  std::vector<float> out;
  if (!synth || sfid_ < 0) return out;

  const int chan = drum ? 9 : 0;
  fluid_synth_all_sounds_off(synth, -1);  // clear any lingering voices
  // Channel controllers shape the timbre/loudness, so set them before the note.
  fluid_synth_cc(synth, chan, 7, volume);
  fluid_synth_cc(synth, chan, 11, expression);
  fluid_synth_cc(synth, chan, 10, pan);
  fluid_synth_program_select(synth, chan, sfid_, drum ? 128 : 0,
                             drum ? 0 : program);
  fluid_synth_noteon(synth, chan, key, velocity);

  std::vector<float> blk(2 * kBlock);
  long remaining = std::max<long>(duration_frames, 1);
  while (remaining > 0) {
    const int n = static_cast<int>(std::min<long>(kBlock, remaining));
    fluid_synth_write_float(synth, n, blk.data(), 0, 2, blk.data(), 1, 2);
    out.insert(out.end(), blk.begin(), blk.begin() + 2 * n);
    remaining -= n;
  }

  fluid_synth_noteoff(synth, chan, key);
  const long tail_cap = static_cast<long>(rate_) * 3;  // 3 s release ceiling
  for (long tail = 0; tail < tail_cap; tail += kBlock) {
    fluid_synth_write_float(synth, kBlock, blk.data(), 0, 2, blk.data(), 1, 2);
    out.insert(out.end(), blk.begin(), blk.end());
    float peak = 0.0f;
    for (float v : blk) peak = std::max(peak, std::fabs(v));
    if (peak < kSilence) break;
  }
  if (gain != 1.0f)
    for (float& v : out) v *= gain;
  return out;
}

}  // namespace circus2bmson
