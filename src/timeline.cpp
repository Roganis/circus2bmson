#include "circus2bmson/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace circus2bmson {
namespace {

constexpr int kResolution = 240;
constexpr int kPulsesPerRow = kResolution / 4;  // 60: one row == a 1/16 note
constexpr int kRowsPerMeasure = 16;             // 4/4 bar
constexpr int kDefaultSpeed = 6;
constexpr int kDefaultTempo = 125;
constexpr long kMaxRows = 500000;               // safety backstop

double effective_bpm(int speed, int tempo) {
  return 6.0 * tempo / speed;
}

}  // namespace

Timeline build_timeline(const Module& mod, const TimelineOptions& opts) {
  Timeline tl;
  tl.resolution = kResolution;
  tl.pulses_per_row = kPulsesPerRow;
  const int max_loops = std::max(1, opts.max_loops);

  int speed = kDefaultSpeed;
  int tempo = kDefaultTempo;
  int last_speed = speed;
  int last_tempo = tempo;

  std::vector<std::uint8_t> last_sample(mod.channels, 0);

  // Pattern-loop (E6x) state -- global approximation of the per-channel registers.
  int ploop_row = 0;
  int ploop_count = 0;

  // Song loops are counted on backward order jumps; intra-pattern E6x loops
  // stay within one order position and so never count as a song loop.
  int loops_done = 0;

  int o = 0;
  int r = 0;
  bool first_row = true;

  while (o >= 0 && o < mod.song_length) {
    if (tl.emitted_rows >= kMaxRows) {
      tl.truncated = true;
      break;
    }

    const ModPattern& pat = mod.patterns[mod.order[o]];
    const long pulse = tl.emitted_rows * kPulsesPerRow;

    if (r % kRowsPerMeasure == 0) tl.lines.push_back(pulse);

    // Apply speed / tempo (Fxx) for this row, then record any tempo change.
    for (int ch = 0; ch < pat.channels; ++ch) {
      const ModCell& cell = pat.at(r, ch);
      if (cell.effect == 0xF && cell.param != 0) {
        if (cell.param < 0x20) speed = cell.param;
        else tempo = cell.param;
      }
    }
    if (first_row) {
      tl.init_bpm = effective_bpm(speed, tempo);
      last_speed = speed;
      last_tempo = tempo;
      first_row = false;
    } else if (speed != last_speed || tempo != last_tempo) {
      tl.bpm_events.push_back({pulse, effective_bpm(speed, tempo)});
      last_speed = speed;
      last_tempo = tempo;
    }

    // Emit note-ons. A cell with a period starts a note; a bare sample number
    // just latches the channel's current sample.
    for (int ch = 0; ch < pat.channels; ++ch) {
      const ModCell& cell = pat.at(r, ch);
      if (cell.sample != 0) last_sample[ch] = cell.sample;
      if (cell.period != 0) {
        const std::uint8_t s = cell.sample != 0 ? cell.sample : last_sample[ch];
        // Note delay (EDx) shifts the note part-way into the row.
        long sub = 0;
        if (cell.effect == 0xE && (cell.param >> 4) == 0xD) {
          const int d = std::min<int>(cell.param & 0x0F, speed - 1);
          if (d > 0)
            sub = std::min<long>(
                std::lround(static_cast<double>(d) / speed * kPulsesPerRow),
                kPulsesPerRow - 1);
        }
        tl.notes.push_back({o, r, ch, s, cell.period, pulse + sub});
      }
    }

    tl.total_seconds += speed * 2.5 / tempo;
    ++tl.emitted_rows;

    // Decode control flow on this row.
    bool has_posjump = false, has_break = false, e6_jump = false;
    int posjump = 0, break_row = 0;
    for (int ch = 0; ch < pat.channels; ++ch) {
      const ModCell& cell = pat.at(r, ch);
      const std::uint8_t e = cell.effect, x = cell.param;
      if (e == 0xB) {
        has_posjump = true;
        posjump = x;
      } else if (e == 0xD) {
        has_break = true;
        break_row = (x >> 4) * 10 + (x & 0x0F);
      } else if (e == 0xE && (x >> 4) == 0x6) {
        const int sub = x & 0x0F;
        if (sub == 0) {
          ploop_row = r;
        } else if (ploop_count == 0) {
          ploop_count = sub;
          e6_jump = true;
        } else {
          --ploop_count;
          e6_jump = ploop_count > 0;
        }
      } else if (e == 0xE && (x >> 4) == 0xE) {
        tl.unsupported_flow++;  // EEx pattern delay: timing not modelled yet
      }
    }

    // Compute the next (order, row).
    int next_o = o, next_r = r + 1;
    if (next_r >= pat.rows) {
      next_o = o + 1;
      next_r = 0;
    }
    if (has_break) {
      next_o = o + 1;
      next_r = break_row;
    }
    if (has_posjump) {
      next_o = posjump;
      next_r = has_break ? break_row : 0;
    }
    if (e6_jump) {  // intra-pattern loop overrides normal advance
      next_o = o;
      next_r = ploop_row;
    }
    if (next_r >= pat.rows) next_r = 0;

    // Song-loop accounting: a strictly backward order jump is one loop pass.
    if (next_o >= 0 && next_o < mod.song_length && next_o < o) {
      if (loops_done >= max_loops - 1) break;  // unrolled enough; stop here
      ++loops_done;
    }

    o = next_o;
    r = next_r;
  }

  tl.total_pulses = tl.emitted_rows * kPulsesPerRow;
  tl.loops_played = loops_done;
  return tl;
}

}  // namespace circus2bmson
