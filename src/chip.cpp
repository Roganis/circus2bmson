#include "circus2bmson/chip.hpp"

#include <stdexcept>
#include <string>

#ifdef C2B_HAVE_LIBGME
#include <algorithm>
#include <vector>

#include <gme/gme.h>

#include "circus2bmson/onset.hpp"
#endif

namespace circus2bmson {

#ifdef C2B_HAVE_LIBGME

bool chip_supported() { return true; }

namespace {
// gme.h pulls in blargg_source.h, which #defines `check`; avoid that name.
void throw_on_err(const char* err) {  // gme_err_t is non-NULL on failure
  if (err) throw std::runtime_error(std::string("libgme: ") + err);
}
const char* or_empty(const char* s) { return s ? s : ""; }
}  // namespace

ChipScan scan_chip(const std::vector<std::uint8_t>& bytes, int sample_rate) {
  if (bytes.empty()) throw std::runtime_error("scan_chip: empty input");

  Music_Emu* emu = nullptr;
  throw_on_err(gme_open_data(bytes.data(), static_cast<long>(bytes.size()), &emu,
                      sample_rate));
  // Keep absolute timing: don't skip leading silence (also disables gme's
  // silence-based end detection -- we bound playback by the track length below).
  gme_ignore_silence(emu, 1);

  ChipScan scan;
  scan.sample_rate = sample_rate;
  if (gme_type_t t = gme_type(emu)) {
    scan.type = or_empty(gme_type_extension(t));
    scan.system = or_empty(gme_type_system(t));
  }
  const int voices = gme_voice_count(emu);

  long total = 0;
  gme_info_t* info = nullptr;
  if (!gme_track_info(emu, &info, 0) && info) {
    if (info->play_length > 0)
      total = static_cast<long>(static_cast<long long>(info->play_length) *
                                sample_rate / 1000);
    if (scan.system.empty()) scan.system = or_empty(info->system);
    scan.game = or_empty(info->game);
    scan.song = or_empty(info->song);
    scan.author = or_empty(info->author);
    gme_free_info(info);
  }
  const long cap = static_cast<long>(sample_rate) * 600;  // 10-min safety
  if (total <= 0) total = cap;
  total = std::min(total, cap);
  scan.total_frames = total;

  // Render each voice alone (mute the rest) and detect its note onsets.
  constexpr int kChunk = 4096;
  std::vector<short> buf(static_cast<std::size_t>(kChunk) * 2);
  scan.voices.resize(voices);
  for (int v = 0; v < voices; ++v) {
    scan.voices[v].name = or_empty(gme_voice_name(emu, v));
    if (scan.voices[v].name.empty())
      scan.voices[v].name = "Voice " + std::to_string(v + 1);

    throw_on_err(gme_start_track(emu, 0));
    gme_mute_voices(emu, ((1 << voices) - 1) & ~(1 << v));  // mute all but v

    std::vector<float> stem;
    stem.reserve(static_cast<std::size_t>(total) * 2);
    long done = 0;
    while (done < total && !gme_track_ended(emu)) {
      const int want = static_cast<int>(std::min<long>(kChunk, total - done));
      throw_on_err(gme_play(emu, want * 2, buf.data()));
      for (int i = 0; i < want * 2; ++i)
        stem.push_back(static_cast<float>(buf[i]) / 32768.0f);
      done += want;
    }
    scan.voices[v].onset_frames = detect_onsets(
        stem.data(), static_cast<long>(stem.size() / 2), sample_rate);
  }

  gme_delete(emu);
  return scan;
}

#else  // !C2B_HAVE_LIBGME

bool chip_supported() { return false; }

ChipScan scan_chip(const std::vector<std::uint8_t>&, int) {
  throw std::runtime_error(
      "chip support not built (libgme was missing at build time)");
}

#endif

}  // namespace circus2bmson
