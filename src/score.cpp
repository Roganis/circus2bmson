#include "circus2bmson/score.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <libopenmpt/libopenmpt.hpp>

namespace circus2bmson {
namespace {

constexpr int kRate = 44100;
constexpr std::size_t kBulkChunk = 4096;
// Safety cap (30 min of unrolled playback) so a pathological module cannot
// run away; also bounds stem memory in the renderer.
constexpr long kMaxFrames = static_cast<long>(kRate) * 1800;

double current_tempo(const openmpt::module& m) {
#if defined(OPENMPT_API_VERSION_AT_LEAST) && OPENMPT_API_VERSION_AT_LEAST(0, 7, 0)
  return m.get_current_tempo2();
#else
  return static_cast<double>(m.get_current_tempo());
#endif
}

}  // namespace

Score read_score(const std::vector<std::uint8_t>& bytes_u8, int max_loops) {
  const std::vector<char> bytes(bytes_u8.begin(), bytes_u8.end());
  openmpt::module m(bytes);

  Score s;
  s.sample_rate = kRate;
  s.repeat_count = std::max(0, max_loops - 1);
  m.set_repeat_count(s.repeat_count);

  s.title = m.get_metadata("title");
  s.format = m.get_metadata("type");
  s.channels = m.get_num_channels();
  s.use_instruments = m.get_num_instruments() > 0;
  s.key_names = s.use_instruments ? m.get_instrument_names()
                                  : m.get_sample_names();

  // S3M/IT 'T' with param < 0x20 slides tempo per tick, which can shorten a
  // row below its start-of-row estimate; widen the single-step margin there.
  const bool tempo_slide_format =
      s.format == "s3m" || s.format == "it" || s.format == "mptm";
  auto row_has_tempo_slide = [&](int pattern, int row) {
    if (!tempo_slide_format) return false;
    for (int ch = 0; ch < s.channels; ++ch) {
      const std::string fx = m.format_pattern_row_channel_command(
          pattern, row, ch, openmpt::module::command_effect);
      if (!fx.empty() && fx[0] == 'T') {
        const std::uint8_t p = m.get_pattern_row_channel_command(
            pattern, row, ch, openmpt::module::command_parameter);
        if (p != 0 && p < 0x20) return true;
      }
    }
    return false;
  };

  // --- Play through once, pinning each row's onset frame exactly. ---
  // Fast-forward to just before the row's expected end (from its speed/tempo),
  // then step one frame at a time to find the transition; exact onsets keep
  // repeats of the same note byte-identical so they deduplicate.
  struct Raw {
    int order, row, pattern;
    long frame;
    double speed, tempo;
  };
  std::vector<Raw> raw;
  std::vector<float> buf(2 * kBulkChunk);
  long frame = 0;

  int cur_o = m.get_current_order();
  int cur_r = m.get_current_row();
  // Prime with one frame so row 0's tick-0 commands (e.g. an Fxx tempo set on
  // the very first row) are applied before we sample speed/tempo; a stale
  // estimate here would overshoot the first row boundaries.
  if (m.read_interleaved_stereo(kRate, 1, buf.data()) == 1) frame = 1;
  double speed = std::max(1, m.get_current_speed());
  double tempo = std::max(1.0, current_tempo(m));
  raw.push_back({cur_o, cur_r, m.get_order_pattern(cur_o), 0, speed, tempo});

  bool ended = false;
  while (!ended && !s.truncated) {
    const long est = std::lround(kRate * 2.5 * speed / tempo);
    long margin = 256;
    if (row_has_tempo_slide(raw.back().pattern, cur_r))
      margin = std::max<long>(margin, est / 2);
    const long ff = std::max<long>(0, est - margin);

    for (long done = 0; done < ff;) {
      const std::size_t want =
          static_cast<std::size_t>(std::min<long>(ff - done, kBulkChunk));
      const std::size_t n = m.read_interleaved_stereo(kRate, want, buf.data());
      if (n == 0) { ended = true; break; }
      done += static_cast<long>(n);
      frame += static_cast<long>(n);
    }
    if (ended) break;

    // If the row already changed inside the fast-forward (e.g. an extreme
    // tempo slide), we cannot recover the exact onset; record it coarsely.
    if (m.get_current_order() != cur_o || m.get_current_row() != cur_r) {
      ++s.coarse_rows;
      cur_o = m.get_current_order();
      cur_r = m.get_current_row();
      speed = std::max(1, m.get_current_speed());
      tempo = std::max(1.0, current_tempo(m));
      raw.push_back({cur_o, cur_r, m.get_order_pattern(cur_o), frame, speed, tempo});
      continue;
    }

    for (;;) {
      const std::size_t n = m.read_interleaved_stereo(kRate, 1, buf.data());
      if (n == 0) { ended = true; break; }
      ++frame;
      if (frame >= kMaxFrames) { s.truncated = true; break; }
      const int o = m.get_current_order();
      const int r = m.get_current_row();
      if (o != cur_o || r != cur_r) {
        cur_o = o;
        cur_r = r;
        speed = std::max(1, m.get_current_speed());
        tempo = std::max(1.0, current_tempo(m));
        raw.push_back({o, r, m.get_order_pattern(o), frame - 1, speed, tempo});
        break;
      }
    }
  }
  s.total_frames = frame;
  s.total_seconds = static_cast<double>(frame) / kRate;

  // libopenmpt reports a terminal position wrap back to the song start once
  // playback ends; drop that trailing backward marker so the first row's
  // note-ons are not re-sliced.
  if (raw.size() >= 2) {
    const Raw& last = raw.back();
    const Raw& prev = raw[raw.size() - 2];
    if (last.order < prev.order ||
        (last.order == prev.order && last.row <= prev.row))
      raw.pop_back();
  }
  if (raw.empty()) throw std::runtime_error("module produced no playback");

  // --- Derive the bmson timeline. ---
  s.rows.reserve(raw.size());
  double cur_bpm = 0.0;
  std::vector<int> latched_instr(s.channels, 0);

  for (std::size_t i = 0; i < raw.size(); ++i) {
    const Raw& rw = raw[i];
    const long pulse = static_cast<long>(i) * s.pulses_per_row;
    s.rows.push_back({rw.order, rw.row, rw.pattern, rw.frame});

    if (i > 0 && rw.order != raw[i - 1].order && rw.order < raw[i - 1].order)
      ++s.backward_jumps;

    // Bar lines: pattern starts and every 16 rows within a pattern.
    if (rw.row % 16 == 0 || i == 0 || rw.order != raw[i - 1].order)
      s.lines.push_back(pulse);

    // Per-row BPM: trust 6*tempo/speed when it matches the measured duration
    // (stable, rational); otherwise use the measured duration itself, which
    // covers pattern delay, mid-row tempo slides, etc. The last row has no
    // next boundary -- the stream tail is note release, not row time -- so it
    // always uses the nominal value.
    double bpm = 6.0 * rw.tempo / rw.speed;
    if (i + 1 < raw.size()) {
      const long dur = raw[i + 1].frame - rw.frame;
      const double expected = kRate * 2.5 * rw.speed / rw.tempo;
      if (dur > 0 && std::fabs(dur - expected) > 4.0)
        bpm = 15.0 * kRate / static_cast<double>(dur);
    }
    if (i == 0) {
      s.init_bpm = bpm;
      cur_bpm = bpm;
    } else if (std::fabs(bpm - cur_bpm) > 1e-6) {
      s.bpm_events.push_back({pulse, bpm});
      cur_bpm = bpm;
    }

    // Note-ons (instrument numbers latch like tracker playback).
    for (int ch = 0; ch < s.channels; ++ch) {
      const int iv = m.get_pattern_row_channel_command(
          rw.pattern, rw.row, ch, openmpt::module::command_instrument);
      if (iv != 0) latched_instr[ch] = iv;
      const int note = m.get_pattern_row_channel_command(
          rw.pattern, rw.row, ch, openmpt::module::command_note);
      if (note >= 1 && note <= 128)
        s.notes.push_back(
            {static_cast<int>(i), ch, note, latched_instr[ch], pulse});
    }
  }
  s.total_pulses = static_cast<long>(s.rows.size()) * s.pulses_per_row;
  return s;
}

}  // namespace circus2bmson
