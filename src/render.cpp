#include "circus2bmson/render.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dr_libs/dr_wav.h>
#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>

#include "oggenc.hpp"

namespace circus2bmson {
namespace {

constexpr std::size_t kBulkChunk = 4096;
constexpr float kSilence = 1.0e-4f;  // trailing-silence trim threshold

std::int16_t to_i16(float f) {
  float v = f * 32767.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  return static_cast<std::int16_t>(std::lrintf(v));
}

std::string pad2(int v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%02d", v);
  return b;
}

// Reduce an instrument/sample name to a short, filename-safe token (may be empty).
std::string sanitize_name(const std::string& s, std::size_t maxlen = 12) {
  std::string out;
  bool last_us = false;
  for (unsigned char ch : s) {
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
      last_us = false;
    } else if (!out.empty() && !last_us) {
      out.push_back('_');
      last_us = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.size() > maxlen) {
    out.resize(maxlen);
    while (!out.empty() && out.back() == '_') out.pop_back();
  }
  return out;
}

// libopenmpt note value (1 = C-0) -> display name, e.g. 61 -> "C-5".
std::string note_value_to_name(int note) {
  if (note < 1) return "x";
  const int n = note - 1;
  static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};
  const std::string nm = kNames[n % 12];
  const int octave = n / 12;
  return nm.size() == 1 ? nm + "-" + std::to_string(octave)
                        : nm + std::to_string(octave);
}

// Descriptive keysound base name (without extension or collision suffix).
std::string keysound_base(const Score& score, KeysoundNaming naming,
                          int instrument, int channel, int note) {
  const std::string s = "s" + pad2(instrument);
  std::string nm;
  if (instrument >= 1 &&
      instrument <= static_cast<int>(score.key_names.size()))
    nm = sanitize_name(score.key_names[instrument - 1]);
  const std::string instr = nm.empty() ? s : s + "_" + nm;
  const std::string ch = "ch" + pad2(channel + 1);  // 1-based for display
  const std::string nn = note_value_to_name(note);
  return naming == KeysoundNaming::Lane ? ch + "_" + instr + "_" + nn
                                        : instr + "_" + ch + "_" + nn;
}

// Render one pattern channel in isolation to interleaved stereo float (bulk).
// channel < 0 -> full mix (mute nothing).
std::vector<float> render_channel(const std::vector<char>& bytes, int channel,
                                  const Score& score, bool volume_ramping) {
  openmpt::module_ext mod(bytes);
  mod.set_repeat_count(score.repeat_count);
  // By default disable libopenmpt's anti-click volume ramping: it makes a
  // note's first ~1 ms depend on the previous note's tail, which stops
  // otherwise-identical notes from deduplicating. Amiga Paula had no ramping,
  // so off is also more authentic.
  if (!volume_ramping)
    mod.set_render_param(openmpt::module::RENDER_VOLUMERAMPING_STRENGTH, 0);
  if (channel >= 0) {
    auto* interactive = static_cast<openmpt::ext::interactive*>(
        mod.get_interface(openmpt::ext::interactive_id));
    if (!interactive)
      throw std::runtime_error("libopenmpt interactive interface unavailable");
    for (int j = 0; j < score.channels; ++j)
      interactive->set_channel_mute_status(j, /*mute=*/j != channel);
  }
  std::vector<float> stem;
  std::vector<float> buf(2 * kBulkChunk);
  const long cap = score.total_frames;
  long frames = 0;
  for (;;) {
    const std::size_t n =
        mod.read_interleaved_stereo(score.sample_rate, kBulkChunk, buf.data());
    if (n == 0) break;
    stem.insert(stem.end(), buf.begin(), buf.begin() + 2 * n);
    frames += static_cast<long>(n);
    if (cap > 0 && frames >= cap + score.sample_rate) break;  // safety
  }
  return stem;
}

// Trim trailing silence and quantize a slice to interleaved int16.
std::vector<std::int16_t> make_slice(const std::vector<float>& stem, long start,
                                     long end) {
  long e = end;
  while (e > start) {
    if (std::fabs(stem[2 * (e - 1)]) >= kSilence ||
        std::fabs(stem[2 * (e - 1) + 1]) >= kSilence)
      break;
    --e;
  }
  if (e <= start) e = start + 1;  // keep at least one frame
  std::vector<std::int16_t> pcm;
  pcm.reserve(static_cast<std::size_t>(2 * (e - start)));
  for (long f = start; f < e; ++f) {
    pcm.push_back(to_i16(stem[2 * f]));
    pcm.push_back(to_i16(stem[2 * f + 1]));
  }
  return pcm;
}

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm,
               int rate) {
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_PCM;
  fmt.channels = 2;
  fmt.sampleRate = static_cast<drwav_uint32>(rate);
  fmt.bitsPerSample = 16;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write WAV: " + path);
  drwav_write_pcm_frames(&wav, pcm.size() / 2, pcm.data());
  drwav_uninit(&wav);
}

}  // namespace

RenderResult render_keysounds(const std::vector<std::uint8_t>& bytes_u8,
                              const Score& score, const std::string& out_dir,
                              KeysoundNaming naming, bool volume_ramping,
                              AudioFormat format) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());
  RenderResult rr;
  rr.sample_rate = score.sample_rate;
  rr.note_keysound.assign(score.notes.size(), -1);

  std::unordered_map<std::string, int> dedup;  // raw PCM bytes -> keysound id
  std::set<std::string> used_names;            // keep filenames unique
  std::vector<int> chan_seq(score.channels, 0);
  const char* ext = audio_extension(format);

  for (int c = 0; c < score.channels; ++c) {
    // This channel's note-ons, in play order (score.notes is play-ordered).
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < score.notes.size(); ++i)
      if (score.notes[i].channel == c) idx.push_back(i);
    if (idx.empty()) continue;

    const std::vector<float> stem =
        render_channel(bytes, c, score, volume_ramping);
    const long total_frames = static_cast<long>(stem.size() / 2);

    for (std::size_t k = 0; k < idx.size(); ++k) {
      const ScoreNote& n = score.notes[idx[k]];
      const long start = std::min<long>(n.frame, total_frames - 1);
      const long end =
          k + 1 < idx.size()
              ? std::min<long>(score.notes[idx[k + 1]].frame, total_frames)
              : total_frames;
      if (start < 0 || end <= start) continue;

      std::vector<std::int16_t> pcm = make_slice(stem, start, end);
      ++rr.total_slices;

      std::string raw(reinterpret_cast<const char*>(pcm.data()),
                      pcm.size() * sizeof(std::int16_t));
      auto found = dedup.find(raw);
      int id;
      if (found == dedup.end()) {
        id = static_cast<int>(rr.keysound_names.size());
        std::string base;
        if (naming == KeysoundNaming::Channel) {
          char b[40];
          std::snprintf(b, sizeof(b), "channel%d_%03d", c + 1, ++chan_seq[c]);
          base = b;
        } else {
          base = keysound_base(score, naming, n.instrument, c, n.note);
        }
        std::string name = base + ext;
        for (int v = 2; used_names.count(name); ++v)
          name = base + "_" + std::to_string(v) + ext;
        used_names.insert(name);
        const std::string path = out_dir + "/" + name;
        if (format == AudioFormat::Ogg)
          write_ogg(path, pcm, score.sample_rate);
        else
          write_wav(path, pcm, score.sample_rate);
        rr.keysound_names.push_back(name);
        dedup.emplace(std::move(raw), id);
      } else {
        id = found->second;
      }
      rr.note_keysound[idx[k]] = id;
    }
  }

  rr.unique_keysounds = static_cast<long>(rr.keysound_names.size());
  return rr;
}

double reconstruct_residual_db(const std::vector<std::uint8_t>& bytes_u8,
                               const Score& score, bool volume_ramping) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());
  const std::vector<float> mix =
      render_channel(bytes, -1, score, volume_ramping);
  const long total_frames = static_cast<long>(mix.size() / 2);

  // Slice every channel (dedup irrelevant here) and remember onset + audio.
  std::vector<std::vector<std::int16_t>> slices(score.notes.size());
  std::vector<long> onsets(score.notes.size(), -1);
  for (int c = 0; c < score.channels; ++c) {
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < score.notes.size(); ++i)
      if (score.notes[i].channel == c) idx.push_back(i);
    if (idx.empty()) continue;
    const std::vector<float> stem =
        render_channel(bytes, c, score, volume_ramping);
    const long frames = static_cast<long>(stem.size() / 2);
    for (std::size_t k = 0; k < idx.size(); ++k) {
      const long start = std::min<long>(score.notes[idx[k]].frame, frames - 1);
      const long end =
          k + 1 < idx.size()
              ? std::min<long>(score.notes[idx[k + 1]].frame, frames)
              : frames;
      if (start < 0 || end <= start) continue;
      slices[idx[k]] = make_slice(stem, start, end);
      onsets[idx[k]] = start;
    }
  }

  // Place each slice back at its onset and compare against the mix.
  std::vector<float> recon(mix.size(), 0.0f);
  for (std::size_t i = 0; i < slices.size(); ++i) {
    if (onsets[i] < 0) continue;
    const std::vector<std::int16_t>& ks = slices[i];
    const long frames = static_cast<long>(ks.size() / 2);
    for (long f = 0; f < frames && (onsets[i] + f) < total_frames; ++f) {
      recon[2 * (onsets[i] + f)] += ks[2 * f] / 32768.0f;
      recon[2 * (onsets[i] + f) + 1] += ks[2 * f + 1] / 32768.0f;
    }
  }

  double sq_err = 0.0, sq_sig = 0.0;
  for (std::size_t i = 0; i < mix.size(); ++i) {
    const double d = static_cast<double>(mix[i]) - static_cast<double>(recon[i]);
    sq_err += d * d;
    sq_sig += static_cast<double>(mix[i]) * static_cast<double>(mix[i]);
  }
  const std::size_t denom = std::max<std::size_t>(1, mix.size());
  const double rms_err = std::sqrt(sq_err / denom);
  const double rms_sig = std::sqrt(sq_sig / denom);
  return (rms_sig > 0.0) ? 20.0 * std::log10((rms_err + 1e-30) / rms_sig)
                         : -1000.0;
}

}  // namespace circus2bmson
