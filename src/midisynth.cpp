#include "midisynth.hpp"

#include <algorithm>
#include <cmath>

#include <fluidsynth.h>

namespace circus2bmson {
namespace {
constexpr int kBlock = 1024;
constexpr float kSilence = 1.0e-4f;
}  // namespace

MidiSynth::MidiSynth(const std::string& soundfont_path, int sample_rate)
    : rate_(sample_rate) {
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
  if (synth) {
    sfid_ = fluid_synth_sfload(synth, soundfont_path.c_str(), 1);
  }
}

MidiSynth::~MidiSynth() {
  if (synth_) delete_fluid_synth(static_cast<fluid_synth_t*>(synth_));
  if (settings_) delete_fluid_settings(static_cast<fluid_settings_t*>(settings_));
}

std::vector<float> MidiSynth::render_note(int program, int key, int velocity,
                                          bool drum, long duration_frames) {
  auto* synth = static_cast<fluid_synth_t*>(synth_);
  std::vector<float> out;
  if (!synth || sfid_ < 0) return out;

  const int chan = drum ? 9 : 0;
  fluid_synth_all_sounds_off(synth, -1);  // clear any lingering voices
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
  return out;
}

}  // namespace circus2bmson
