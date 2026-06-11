#include "circus2bmson/render.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr std::size_t kMarkerChunk = 64;    // small: sample-accurate row onsets
constexpr std::size_t kBulkChunk = 4096;    // large: audio-only passes
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

// Render one channel in isolation (all others muted) to interleaved stereo
// float. When `markers` is non-null, poll the row position with a small chunk
// to record each row's onset frame.
std::vector<float> render_channel(const std::vector<char>& bytes, int channel,
                                  int num_channels,
                                  std::vector<RowMark>* markers) {
  openmpt::module_ext mod(bytes);
  mod.set_repeat_count(0);
  if (channel >= 0) {  // channel < 0 -> full mix (mute nothing)
    auto* interactive = static_cast<openmpt::ext::interactive*>(
        mod.get_interface(openmpt::ext::interactive_id));
    if (!interactive)
      throw std::runtime_error("libopenmpt interactive interface unavailable");
    for (int j = 0; j < num_channels; ++j)
      interactive->set_channel_mute_status(j, /*mute=*/j != channel);
  }

  const std::size_t chunk = markers ? kMarkerChunk : kBulkChunk;
  std::vector<float> stem;
  std::vector<float> buf(2 * chunk);
  long frames = 0;
  int prev_order = mod.get_current_order();
  int prev_row = mod.get_current_row();
  if (markers) {
    markers->clear();
    markers->push_back({0, prev_order, prev_row});
  }

  for (;;) {
    const std::size_t n =
        mod.read_interleaved_stereo(kRate, chunk, buf.data());
    if (n == 0) break;
    stem.insert(stem.end(), buf.begin(), buf.begin() + 2 * n);
    if (markers) {
      const int o = mod.get_current_order();
      const int r = mod.get_current_row();
      if (o != prev_order || r != prev_row) {
        markers->push_back({frames, o, r});  // start-of-chunk: <= true onset
        prev_order = o;
        prev_row = r;
      }
    }
    frames += static_cast<long>(n);
  }
  // libopenmpt reports a terminal position wrap back to the song start once
  // playback ends; drop that trailing backward marker so the first row's
  // note-ons are not re-sliced (which would also mis-map their keysounds).
  if (markers && markers->size() >= 2) {
    const RowMark& last = markers->back();
    const RowMark& prev = (*markers)[markers->size() - 2];
    if (last.order < prev.order ||
        (last.order == prev.order && last.row <= prev.row))
      markers->pop_back();
  }
  if (markers && std::getenv("C2B_DEBUG_MARKERS")) {
    std::fprintf(stderr, "[markers] count=%zu  last:\n", markers->size());
    for (std::size_t i = markers->size() > 8 ? markers->size() - 8 : 0;
         i < markers->size(); ++i)
      std::fprintf(stderr, "   #%zu frame=%ld order=%d row=%d\n", i,
                   (*markers)[i].frame, (*markers)[i].order, (*markers)[i].row);
  }
  return stem;
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
                              KeysoundNaming naming) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());
  RenderResult rr;
  rr.sample_rate = kRate;

  std::vector<RowMark> markers;
  std::unordered_map<std::string, int> dedup;  // raw PCM bytes -> keysound id
  std::set<std::string> used_names;             // keep filenames unique

  for (int c = 0; c < mod.channels; ++c) {
    const std::vector<float> stem =
        render_channel(bytes, c, mod.channels, c == 0 ? &markers : nullptr);
    if (c == 0) {
      rr.render_frames = static_cast<long>(stem.size() / 2);
      rr.marker_rows = static_cast<long>(markers.size());
    }
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
        const std::string base =
            keysound_base(mod, naming, open_sample, c, open_period);
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
                               const Module& mod) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());

  std::vector<RowMark> markers;
  render_channel(bytes, 0, mod.channels, &markers);  // markers only
  const std::vector<float> mix = render_channel(bytes, -1, mod.channels, nullptr);
  const long total_frames = static_cast<long>(mix.size() / 2);

  std::unordered_map<std::string, int> dedup;
  std::vector<std::vector<std::int16_t>> keysounds;
  std::unordered_map<std::uint32_t, int> note_to_id;
  std::unordered_map<std::uint32_t, long> note_onset;

  for (int c = 0; c < mod.channels; ++c) {
    const std::vector<float> stem =
        render_channel(bytes, c, mod.channels, nullptr);
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
