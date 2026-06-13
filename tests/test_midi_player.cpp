// MidiPlayer engine, headless: no audio device needed -- we pull render()
// directly. Verifies channel/instrument discovery, that playback produces
// audio and live levels, and that per-channel gains actually attenuate the mix
// (which also confirms FluidSynth's per-channel audio-group separation, the
// assumption the meters and mixer rely on). Skips when no SoundFont is present.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "check.hpp"
#include "circus2bmson/midiplayer.hpp"

namespace {
std::string find_soundfont() {
  const char* paths[] = {"/usr/share/sounds/sf2/TimGM6mb.sf2",
                         "/usr/share/sounds/sf2/FluidR3_GM.sf2",
                         "/usr/share/sounds/sf2/default-GM.sf2"};
  for (const char* p : paths)
    if (std::filesystem::exists(p)) return p;
  return "";
}

float render_peak(circus2bmson::MidiPlayer& pl, int blocks, int frames) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * 2);
  float peak = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    pl.render(buf.data(), frames);
    for (float v : buf) peak = std::max(peak, std::fabs(v));
  }
  return peak;
}
}  // namespace

int main() {
  using namespace circus2bmson;
  const std::string sf = find_soundfont();
  if (sf.empty()) {
    std::printf("SKIP: no SoundFont installed\n");
    return 0;
  }

  std::ifstream f(std::string(C2B_FIXTURE_DIR) + "/scale.mid", std::ios::binary);
  CHECK(f.good());
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  CHECK(!bytes.empty());

  MidiPlayer pl(sf, bytes, 44100);
  CHECK(pl.ok());
  CHECK(pl.sample_rate() == 44100);
  CHECK(!pl.channels().empty());
  CHECK(!pl.instruments().empty());
  CHECK(pl.duration() > 0.0);

  // Playback produces audio and at least one live channel level.
  pl.seek(0.0);
  pl.play();
  std::vector<float> buf(512 * 2);
  float full = 0.0f, any_level = 0.0f;
  for (int b = 0; b < 80; ++b) {  // ~0.93 s
    pl.render(buf.data(), 512);
    for (float v : buf) full = std::max(full, std::fabs(v));
    for (const auto& t : pl.channels())
      any_level = std::max(any_level, pl.channel_level(t.channel));
  }
  CHECK_MSG(full > 0.01f, "full peak=%.4f", full);
  CHECK_MSG(any_level > 0.0f, "max channel level=%.4f", any_level);

  // Muting every used channel silences the mix: gains apply, and audio is
  // separated per channel (else a muted channel would leak through).
  for (const auto& t : pl.channels()) pl.set_channel_gain(t.channel, 0.0f);
  pl.seek(0.0);
  pl.play();
  const float muted = render_peak(pl, 80, 512);
  CHECK_MSG(muted < full * 0.05f + 1.0e-4f, "muted=%.5f full=%.4f", muted, full);

  // Restoring gain brings the audio back.
  for (const auto& t : pl.channels()) pl.set_channel_gain(t.channel, 1.0f);
  pl.seek(0.0);
  pl.play();
  const float restored = render_peak(pl, 80, 512);
  CHECK_MSG(restored > 0.01f, "restored peak=%.4f", restored);

  const MidiMix mix = pl.snapshot();
  CHECK(mix.honor_cc == true);

  REPORT_AND_RETURN();
}
