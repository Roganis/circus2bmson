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

namespace circus2bmson {
namespace {

constexpr int kRate = 44100;
constexpr std::size_t kBulkChunk = 4096;    // bulk render chunk
constexpr float kSilence = 1.0e-4f;         // trailing-silence trim threshold

// A row in libopenmpt playback order with its starting frame offset.
struct RowMark {
  long frame;
  int order;
  int row;
};

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

// Reduce a MOD sample name to a short, filename-safe token (may be empty).
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

// Amiga period -> note name (period 856 == C-1, halving each octave).
std::string period_to_note_name(int period) {
  if (period <= 0) return "x";
  const int n = static_cast<int>(std::lround(12.0 * std::log2(856.0 / period)));
  const int pc = ((n % 12) + 12) % 12;
  const int octave = 1 + static_cast<int>(std::floor(n / 12.0));
  static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};
  const std::string nm = kNames[pc];
  return nm.size() == 1 ? nm + "-" + std::to_string(octave)
                        : nm + std::to_string(octave);
}

// Descriptive keysound base name (without extension or collision suffix).
std::string keysound_base(const Module& mod, KeysoundNaming naming, int sample,
                          int channel, int period) {
  const std::string s = "s" + pad2(sample);
  std::string nm;
  if (sample >= 1 && sample <= static_cast<int>(mod.samples.size()))
    nm = sanitize_name(mod.samples[sample - 1].name);
  const std::string instr = nm.empty() ? s : s + "_" + nm;
  const std::string ch = "ch" + pad2(channel + 1);  // 1-based for display
  const std::string note = period_to_note_name(period);
  return naming == KeysoundNaming::Lane ? ch + "_" + instr + "_" + note
                                        : instr + "_" + ch + "_" + note;
}

// Render one channel in isolation to interleaved stereo float (bulk; no
// position polling). channel < 0 -> full mix (mute nothing).
std::vector<float> render_channel(const std::vector<char>& bytes, int channel,
                                  int num_channels, bool volume_ramping) {
  openmpt::module_ext mod(bytes);
  mod.set_repeat_count(0);
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
    for (int j = 0; j < num_channels; ++j)
      interactive->set_channel_mute_status(j, /*mute=*/j != channel);
  }
  std::vector<float> stem;
  std::vector<float> buf(2 * kBulkChunk);
  for (;;) {
    const std::size_t n =
        mod.read_interleaved_stereo(kRate, kBulkChunk, buf.data());
    if (n == 0) break;
    stem.insert(stem.end(), buf.begin(), buf.begin() + 2 * n);
  }
  return stem;
}

// Sample-accurate onset frame for every played row. libopenmpt has no per-row
// callback, so we fast-forward to just before each row's expected end (using
// that row's own speed/tempo) and then step one frame at a time to pin the
// exact transition. Exact onsets keep repeats of the same note byte-identical
// so they deduplicate, while the fast-forward keeps it quick.
std::vector<RowMark> compute_row_markers(const std::vector<char>& bytes,
                                         const Module& mod) {
  openmpt::module m(bytes);
  m.set_repeat_count(0);

  int speed = 6, tempo = 125;
  auto apply_fxx = [&](int o, int r) {
    const ModPattern& pat = mod.patterns[mod.order[o]];
    for (int ch = 0; ch < pat.channels; ++ch) {
      const ModCell& c = pat.at(r, ch);
      if (c.effect == 0xF && c.param != 0) {
        if (c.param < 0x20) speed = c.param;
        else tempo = c.param;
      }
    }
  };

  std::vector<RowMark> markers;
  std::vector<float> buf(2 * kBulkChunk);
  long frame = 0;
  int prev_o = m.get_current_order();
  int prev_r = m.get_current_row();
  markers.push_back({0, prev_o, prev_r});
  apply_fxx(prev_o, prev_r);

  for (;;) {
    // Fast-forward to a safe margin before the expected next boundary.
    long est = std::lround(kRate * 2.5 * speed / tempo) - 256;
    if (est < 0) est = 0;
    bool ended = false;
    for (long done = 0; done < est;) {
      const std::size_t want =
          static_cast<std::size_t>(std::min<long>(est - done, kBulkChunk));
      const std::size_t n = m.read_interleaved_stereo(kRate, want, buf.data());
      if (n == 0) { ended = true; break; }
      done += static_cast<long>(n);
      frame += static_cast<long>(n);
    }
    if (ended) break;
    // Step one frame at a time to pin the exact row transition.
    for (;;) {
      const std::size_t n = m.read_interleaved_stereo(kRate, 1, buf.data());
      if (n == 0) { ended = true; break; }
      ++frame;
      const int o = m.get_current_order();
      const int r = m.get_current_row();
      if (o != prev_o || r != prev_r) {
        markers.push_back({frame - 1, o, r});
        prev_o = o;
        prev_r = r;
        apply_fxx(o, r);
        break;
      }
    }
    if (ended) break;
  }

  // libopenmpt reports a terminal position wrap back to the song start once
  // playback ends; drop that trailing backward marker.
  if (markers.size() >= 2) {
    const RowMark& last = markers.back();
    const RowMark& prev = markers[markers.size() - 2];
    if (last.order < prev.order ||
        (last.order == prev.order && last.row <= prev.row))
      markers.pop_back();
  }
  return markers;
}

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm) {
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_PCM;
  fmt.channels = 2;
  fmt.sampleRate = kRate;
  fmt.bitsPerSample = 16;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write WAV: " + path);
  drwav_write_pcm_frames(&wav, pcm.size() / 2, pcm.data());
  drwav_uninit(&wav);
}

}  // namespace

RenderResult render_keysounds(const std::vector<std::uint8_t>& bytes_u8,
                              const Module& mod, const std::string& out_dir,
                              KeysoundNaming naming, bool volume_ramping) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());
  RenderResult rr;
  rr.sample_rate = kRate;

  const std::vector<RowMark> markers = compute_row_markers(bytes, mod);
  rr.marker_rows = static_cast<long>(markers.size());
  std::unordered_map<std::string, int> dedup;  // raw PCM bytes -> keysound id
  std::set<std::string> used_names;             // keep filenames unique
  std::vector<int> chan_seq(mod.channels, 0);   // per-channel counter (Channel)

  for (int c = 0; c < mod.channels; ++c) {
    const std::vector<float> stem =
        render_channel(bytes, c, mod.channels, volume_ramping);
    if (c == 0) rr.render_frames = static_cast<long>(stem.size() / 2);
    const long total_frames = static_cast<long>(stem.size() / 2);

    // Slice the channel at its note-ons; each slice runs to the next note-on.
    bool open = false;
    long start = 0;
    std::uint32_t key = 0;
    int open_sample = 0;
    int open_period = 0;
    std::uint8_t last_sample = 0;  // channel's latched sample

    auto flush = [&](long end) {
      if (!open) return;
      open = false;
      // Trim trailing silence.
      long e = end;
      while (e > start) {
        const float l = stem[2 * (e - 1)];
        const float r = stem[2 * (e - 1) + 1];
        if (std::fabs(l) >= kSilence || std::fabs(r) >= kSilence) break;
        --e;
      }
      if (e <= start) e = start + 1;  // keep at least one frame

      std::vector<std::int16_t> pcm;
      pcm.reserve(static_cast<std::size_t>(2 * (e - start)));
      for (long f = start; f < e; ++f) {
        pcm.push_back(to_i16(stem[2 * f]));
        pcm.push_back(to_i16(stem[2 * f + 1]));
      }
      ++rr.total_slices;

      std::string raw(reinterpret_cast<const char*>(pcm.data()),
                      pcm.size() * sizeof(std::int16_t));
      auto found = dedup.find(raw);
      int id;
      if (found == dedup.end()) {
        id = static_cast<int>(rr.keysound_names.size());
        std::string base;
        if (naming == KeysoundNaming::Channel) {
          char b[24];
          std::snprintf(b, sizeof(b), "channel%d_%03d", c + 1, ++chan_seq[c]);
          base = b;
        } else {
          base = keysound_base(mod, naming, open_sample, c, open_period);
        }
        std::string name = base + ".wav";
        for (int k = 2; used_names.count(name); ++k)
          name = base + "_" + std::to_string(k) + ".wav";
        used_names.insert(name);
        write_wav(out_dir + "/" + name, pcm);
        rr.keysound_names.push_back(name);
        dedup.emplace(std::move(raw), id);
      } else {
        id = found->second;
      }
      rr.note_to_keysound[key] = id;
    };

    for (const RowMark& mk : markers) {
      const ModCell& cell = mod.patterns[mod.order[mk.order]].at(mk.row, c);
      if (cell.sample != 0) last_sample = cell.sample;
      if (cell.period != 0) {
        flush(mk.frame);
        open = true;
        start = mk.frame;
        key = note_key(mk.order, mk.row, c);
        open_period = cell.period;
        open_sample = cell.sample != 0 ? cell.sample : last_sample;
      }
    }
    flush(total_frames);
  }

  rr.unique_keysounds = static_cast<long>(rr.keysound_names.size());
  return rr;
}

double reconstruct_residual_db(const std::vector<std::uint8_t>& bytes_u8,
                               const Module& mod, bool volume_ramping) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());

  const std::vector<RowMark> markers = compute_row_markers(bytes, mod);
  const std::vector<float> mix =
      render_channel(bytes, -1, mod.channels, volume_ramping);
  const long total_frames = static_cast<long>(mix.size() / 2);

  std::unordered_map<std::string, int> dedup;
  std::vector<std::vector<std::int16_t>> keysounds;
  std::unordered_map<std::uint32_t, int> note_to_id;
  std::unordered_map<std::uint32_t, long> note_onset;

  for (int c = 0; c < mod.channels; ++c) {
    const std::vector<float> stem =
        render_channel(bytes, c, mod.channels, volume_ramping);
    bool open = false;
    long start = 0;
    std::uint32_t key = 0;
    auto flush = [&](long end) {
      if (!open) return;
      open = false;
      long e = end;
      while (e > start) {
        if (std::fabs(stem[2 * (e - 1)]) >= kSilence ||
            std::fabs(stem[2 * (e - 1) + 1]) >= kSilence)
          break;
        --e;
      }
      if (e <= start) e = start + 1;
      std::vector<std::int16_t> pcm;
      pcm.reserve(static_cast<std::size_t>(2 * (e - start)));
      for (long f = start; f < e; ++f) {
        pcm.push_back(to_i16(stem[2 * f]));
        pcm.push_back(to_i16(stem[2 * f + 1]));
      }
      std::string raw(reinterpret_cast<const char*>(pcm.data()),
                      pcm.size() * sizeof(std::int16_t));
      auto it = dedup.find(raw);
      int id;
      if (it == dedup.end()) {
        id = static_cast<int>(keysounds.size());
        keysounds.push_back(std::move(pcm));
        dedup.emplace(std::move(raw), id);
      } else {
        id = it->second;
      }
      note_to_id[key] = id;
      note_onset[key] = start;
    };
    for (const RowMark& mk : markers) {
      const ModCell& cell = mod.patterns[mod.order[mk.order]].at(mk.row, c);
      if (cell.period != 0) {
        flush(mk.frame);
        open = true;
        start = mk.frame;
        key = note_key(mk.order, mk.row, c);
      }
    }
    flush(total_frames);
  }

  // Place each note's keysound back at its onset, via the note->keysound map.
  std::vector<float> recon(mix.size(), 0.0f);
  for (const RowMark& mk : markers)
    for (int c = 0; c < mod.channels; ++c) {
      const ModCell& cell = mod.patterns[mod.order[mk.order]].at(mk.row, c);
      if (cell.period == 0) continue;
      const std::uint32_t key = note_key(mk.order, mk.row, c);
      const std::vector<std::int16_t>& ks = keysounds[note_to_id[key]];
      const long onset = note_onset[key];
      const long frames = static_cast<long>(ks.size() / 2);
      for (long f = 0; f < frames && (onset + f) < total_frames; ++f) {
        recon[2 * (onset + f)] += ks[2 * f] / 32768.0f;
        recon[2 * (onset + f) + 1] += ks[2 * f + 1] / 32768.0f;
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
