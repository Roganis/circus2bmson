#include "circus2bmson/chip.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "chipconv.hpp"

#ifdef C2B_HAVE_LIBGME
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

#include <dr_libs/dr_wav.h>
#include <gme/gme.h>

#include "circus2bmson/bmson.hpp"
#include "circus2bmson/onset.hpp"
#include "circus2bmson/score.hpp"
#include "oggenc.hpp"
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

// Filename-safe token from a voice name: "Square 1" -> "square_1".
std::string sanitize(const std::string& s) {
  std::string out;
  bool underscore = false;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      underscore = false;
    } else if (!out.empty() && !underscore) {
      out.push_back('_');
      underscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

constexpr int kRate = 44100;
constexpr float kSilence = 1.0e-4f;  // trailing-silence trim threshold

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

std::int16_t to_i16(float f) {
  float v = f * 32767.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  return static_cast<std::int16_t>(std::lrintf(v));
}

// Trim trailing silence and quantise stem[start, end) to interleaved int16.
std::vector<std::int16_t> slice_pcm(const std::vector<float>& stem, long start,
                                    long end) {
  const long n = static_cast<long>(stem.size() / 2);
  if (start < 0) start = 0;
  long e = std::min(end, n);
  while (e > start && std::fabs(stem[2 * (e - 1)]) < kSilence &&
         std::fabs(stem[2 * (e - 1) + 1]) < kSilence)
    --e;
  if (e <= start) e = std::min(start + 1, n);
  std::vector<std::int16_t> pcm;
  pcm.reserve(static_cast<std::size_t>(2 * (e - start)));
  for (long f = start; f < e; ++f) {
    pcm.push_back(to_i16(stem[2 * f]));
    pcm.push_back(to_i16(stem[2 * f + 1]));
  }
  return pcm;
}

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm) {
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_PCM;
  fmt.channels = 2;
  fmt.sampleRate = static_cast<drwav_uint32>(kRate);
  fmt.bitsPerSample = 16;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write WAV: " + path);
  drwav_write_pcm_frames(&wav, pcm.size() / 2, pcm.data());
  drwav_uninit(&wav);
}

// Plausible constant BPM from merged, sorted onset times (seconds). Timing is
// exact at any BPM -- this only sets how pulses map to gridlines -- so a crude
// heuristic is safe: take the median onset gap as a 16th note, fold to a sane
// range. Falls back to 150 when there is too little to go on.
double infer_bpm(const std::vector<double>& onsets) {
  std::vector<double> gaps;
  for (std::size_t i = 1; i < onsets.size(); ++i) {
    const double g = onsets[i] - onsets[i - 1];
    if (g > 0.01 && g < 2.0) gaps.push_back(g);
  }
  if (gaps.empty()) return 150.0;
  std::sort(gaps.begin(), gaps.end());
  const double sixteenth = gaps[gaps.size() / 2];
  double bpm = 60.0 / (4.0 * sixteenth);
  while (bpm < 70.0) bpm *= 2.0;
  while (bpm > 280.0) bpm /= 2.0;
  return bpm;
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
  namespace fs = std::filesystem;
  ChipMeta meta;
  const std::vector<std::vector<float>> stems = render_voices(bytes, meta);

  // Onsets per voice, plus a merged list for BPM inference.
  std::vector<std::vector<long>> onsets(stems.size());
  std::vector<double> merged;
  for (std::size_t v = 0; v < stems.size(); ++v) {
    onsets[v] = detect_onsets(stems[v].data(),
                              static_cast<long>(stems[v].size() / 2), kRate);
    for (long f : onsets[v]) merged.push_back(static_cast<double>(f) / kRate);
  }
  std::sort(merged.begin(), merged.end());

  constexpr int kResolution = 480;
  const double bpm = infer_bpm(merged);
  auto pulse_at = [&](long frame) {
    return static_cast<long>(std::llround(static_cast<double>(frame) / kRate *
                                          bpm / 60.0 * kResolution));
  };

  fs::path out_dir(opts.output_dir);
  if (!out_dir.empty()) fs::create_directories(out_dir);
  const char* ext = audio_extension(opts.audio_format);

  // Slice each voice at its onsets, dedup identical PCM, place at exact time.
  std::unordered_map<std::string, int> dedup;  // raw PCM -> keysound id
  std::set<std::string> used_names;
  std::vector<SoundChannel> channels;
  long total_slices = 0, note_count = 0;
  const bool render = opts.render_audio;

  for (std::size_t v = 0; v < stems.size(); ++v) {
    const long n = static_cast<long>(stems[v].size() / 2);
    int seq = 0;
    for (std::size_t k = 0; k < onsets[v].size(); ++k) {
      const long start = onsets[v][k];
      const long end = (k + 1 < onsets[v].size()) ? onsets[v][k + 1] : n;
      if (end <= start) continue;
      ++note_count;
      const long pulse = pulse_at(start);

      int id;
      if (render) {
        std::vector<std::int16_t> pcm = slice_pcm(stems[v], start, end);
        ++total_slices;
        std::string raw(reinterpret_cast<const char*>(pcm.data()),
                        pcm.size() * sizeof(std::int16_t));
        auto found = dedup.find(raw);
        if (found == dedup.end()) {
          id = static_cast<int>(channels.size());
          std::string vn = sanitize(meta.voice_names[v]);
          if (vn.empty()) vn = "voice" + std::to_string(v + 1);
          char b[48];
          std::snprintf(b, sizeof(b), "%s_%03d", vn.c_str(), ++seq);
          std::string base(b);
          std::string name = base + ext;
          for (int x = 2; used_names.count(name); ++x)
            name = base + "_" + std::to_string(x) + ext;
          used_names.insert(name);
          const std::string path = (out_dir / name).string();
          if (opts.audio_format == AudioFormat::Ogg)
            write_ogg(path, pcm, kRate);
          else
            write_wav(path, pcm);
          SoundChannel sc;
          sc.name = name;
          channels.push_back(std::move(sc));
          dedup.emplace(std::move(raw), id);
        } else {
          id = found->second;
        }
      } else {  // skeleton: one channel per voice, no audio files
        id = static_cast<int>(v);
        if (static_cast<std::size_t>(id) >= channels.size())
          channels.resize(id + 1);
        if (channels[id].name.empty())
          channels[id].name = meta.voice_names[v] + ext;
      }
      channels[id].note_pulses.push_back(pulse);
    }
  }

  std::vector<SoundChannel> used;
  for (SoundChannel& c : channels)
    if (!c.note_pulses.empty()) used.push_back(std::move(c));
  std::sort(used.begin(), used.end(),
            [](const SoundChannel& a, const SoundChannel& b) {
              return a.name < b.name;
            });

  // Minimal Score carrying just what build_bmson needs (header + grid).
  Score sc;
  sc.title = !meta.song.empty() ? meta.song
                                : fs::path(input_path).stem().string();
  sc.format = meta.type;
  sc.channels = static_cast<int>(stems.size());
  sc.sample_rate = kRate;
  sc.resolution = kResolution;
  sc.init_bpm = bpm;
  sc.total_frames = meta.total_frames;
  sc.total_seconds = static_cast<double>(meta.total_frames) / kRate;
  sc.total_pulses = pulse_at(meta.total_frames);
  for (long p = 0; p <= sc.total_pulses; p += 4 * kResolution)
    sc.lines.push_back(p);  // a bar line every 4 beats

  const std::string stem = fs::path(input_path).stem().string();
  const fs::path out_path = out_dir / (stem + ".bmson");
  std::ofstream of(out_path, std::ios::binary);
  if (!of) throw std::runtime_error("cannot write output: " + out_path.string());
  of << (render ? build_bmson(sc, used)
                : build_bmson_skeleton(sc, ext));
  of.close();

  ConvertResult r;
  r.bmson_path = out_path.string();
  r.title = sc.title;
  r.format = sc.format;
  r.channels = sc.channels;
  r.note_count = static_cast<std::size_t>(note_count);
  r.line_count = sc.lines.size();
  r.init_bpm = bpm;
  r.total_seconds = sc.total_seconds;
  r.total_pulses = sc.total_pulses;
  r.audio_rendered = render;
  r.total_slices = total_slices;
  r.keysound_count = static_cast<long>(used.size());
  return r;
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
