#include "circus2bmson/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
                              const Module& mod, const std::string& out_dir) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());
  RenderResult rr;
  rr.sample_rate = kRate;

  std::vector<RowMark> markers;
  std::unordered_map<std::string, int> dedup;  // raw PCM bytes -> keysound id

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
        char name[24];
        std::snprintf(name, sizeof(name), "key_%04d.wav", id);
        write_wav(out_dir + "/" + name, pcm);
        rr.keysound_names.emplace_back(name);
        dedup.emplace(std::move(raw), id);
      } else {
        id = found->second;
      }
      rr.note_to_keysound[key] = id;
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
