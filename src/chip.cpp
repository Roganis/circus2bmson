#include "circus2bmson/chip.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "chipconv.hpp"

#ifdef C2B_HAVE_LIBGME
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>

#include <gme/gme.h>

#include "circus2bmson/onset.hpp"
#include "stemconv.hpp"
#endif

namespace circus2bmson {

#ifdef C2B_HAVE_LIBGME

bool chip_supported() { return true; }

namespace {

void throw_on_err(const char* err) {  // gme_err_t is non-NULL on failure
  if (err) throw std::runtime_error(std::string("libgme: ") + err);
}
const char* or_empty(const char* s) { return s ? s : ""; }
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}

constexpr int kRate = 44100;

struct ChipMeta {
  std::string type, system, game, song, author;
  long total_frames = 0;
  std::vector<std::string> voice_names;
};

// Render every emulated voice in isolation (mute the rest), the same per-voice
// stem trick render.cpp uses for libopenmpt channels. Fills `meta`.
std::vector<std::vector<float>> render_voices(const std::vector<std::uint8_t>& bytes,
                                              ChipMeta& meta) {
  if (bytes.empty()) throw std::runtime_error("chip: empty input");

  Music_Emu* emu = nullptr;
  throw_on_err(gme_open_data(bytes.data(), static_cast<long>(bytes.size()), &emu,
                             kRate));
  gme_ignore_silence(emu, 1);  // keep absolute timing (no leading-silence skip)

  if (gme_type_t t = gme_type(emu)) {
    meta.type = lower(or_empty(gme_type_extension(t)));
    meta.system = or_empty(gme_type_system(t));
  }
  const int voices = gme_voice_count(emu);

  long total = 0;
  gme_info_t* info = nullptr;
  if (!gme_track_info(emu, &info, 0) && info) {
    if (info->play_length > 0)
      total = static_cast<long>(static_cast<long long>(info->play_length) *
                                kRate / 1000);
    if (meta.system.empty()) meta.system = or_empty(info->system);
    meta.game = or_empty(info->game);
    meta.song = or_empty(info->song);
    meta.author = or_empty(info->author);
    gme_free_info(info);
  }
  const long cap = static_cast<long>(kRate) * 600;  // 10-min safety
  if (total <= 0) total = cap;
  total = std::min(total, cap);
  meta.total_frames = total;

  std::vector<std::vector<float>> stems(voices);
  meta.voice_names.resize(voices);
  constexpr int kChunk = 4096;
  std::vector<short> buf(static_cast<std::size_t>(kChunk) * 2);
  for (int v = 0; v < voices; ++v) {
    meta.voice_names[v] = or_empty(gme_voice_name(emu, v));
    if (meta.voice_names[v].empty())
      meta.voice_names[v] = "voice" + std::to_string(v + 1);

    throw_on_err(gme_start_track(emu, 0));
    gme_mute_voices(emu, ((1 << voices) - 1) & ~(1 << v));  // mute all but v

    std::vector<float>& stem = stems[v];
    stem.reserve(static_cast<std::size_t>(total) * 2);
    long done = 0;
    while (done < total && !gme_track_ended(emu)) {
      const int want = static_cast<int>(std::min<long>(kChunk, total - done));
      throw_on_err(gme_play(emu, want * 2, buf.data()));
      for (int i = 0; i < want * 2; ++i)
        stem.push_back(static_cast<float>(buf[i]) / 32768.0f);
      done += want;
    }
  }
  gme_delete(emu);
  return stems;
}

}  // namespace

std::vector<std::string> chip_extensions() {
  std::vector<std::string> out;
  for (const gme_type_t* t = gme_type_list(); t && *t; ++t) {
    const char* e = gme_type_extension(*t);
    if (e && *e) out.push_back(lower(e));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

ChipScan scan_chip(const std::vector<std::uint8_t>& bytes, int sample_rate) {
  (void)sample_rate;  // fixed at kRate for now
  ChipMeta meta;
  const std::vector<std::vector<float>> stems = render_voices(bytes, meta);

  ChipScan scan;
  scan.type = meta.type;
  scan.system = meta.system;
  scan.game = meta.game;
  scan.song = meta.song;
  scan.author = meta.author;
  scan.sample_rate = kRate;
  scan.total_frames = meta.total_frames;
  scan.voices.resize(stems.size());
  for (std::size_t v = 0; v < stems.size(); ++v) {
    scan.voices[v].name = meta.voice_names[v];
    scan.voices[v].onset_frames = detect_onsets(
        stems[v].data(), static_cast<long>(stems[v].size() / 2), kRate);
  }
  return scan;
}

ConvertResult convert_chip(const std::vector<std::uint8_t>& bytes,
                           const std::string& input_path,
                           const ConvertOptions& opts) {
  ChipMeta meta;
  StemSong song;
  song.stems = render_voices(bytes, meta);
  song.title = meta.song;  // convert_stems falls back to the filename stem
  song.format = meta.type;
  song.sample_rate = kRate;
  song.total_frames = meta.total_frames;
  song.voice_names = meta.voice_names;
  return convert_stems(song, input_path, opts);
}

bool chip_handles_extension(const std::string& ext) {
  std::string e = ext;
  if (!e.empty() && e[0] == '.') e.erase(0, 1);
  e = lower(e);
  for (const std::string& x : chip_extensions())
    if (x == e) return true;
  return false;
}

#else  // !C2B_HAVE_LIBGME

bool chip_supported() { return false; }
std::vector<std::string> chip_extensions() { return {}; }
bool chip_handles_extension(const std::string&) { return false; }

ChipScan scan_chip(const std::vector<std::uint8_t>&, int) {
  throw std::runtime_error(
      "chip support not built (libgme was missing at build time)");
}
ConvertResult convert_chip(const std::vector<std::uint8_t>&, const std::string&,
                           const ConvertOptions&) {
  throw std::runtime_error(
      "chip support not built (libgme was missing at build time)");
}

#endif

}  // namespace circus2bmson
