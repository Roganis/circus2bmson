#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/mod.hpp"
#include "circus2bmson/timeline.hpp"

namespace {

using namespace circus2bmson;

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

// Build a blank module with `num_patterns` empty 64-row patterns and the given
// order list, for exercising control flow without a real file.
Module make_module(int channels, std::vector<std::uint8_t> order,
                   int num_patterns) {
  Module m;
  m.title = "synthetic";
  m.format_tag = "M.K.";
  m.channels = channels;
  m.song_length = static_cast<int>(order.size());
  m.order = std::move(order);
  m.order.resize(128, 0);
  m.samples.resize(31);
  m.patterns.resize(num_patterns);
  for (ModPattern& p : m.patterns) {
    p.rows = 64;
    p.channels = channels;
    p.cells.resize(static_cast<std::size_t>(64) * channels);
  }
  return m;
}

void set_cell(Module& m, int pat, int row, int ch, std::uint16_t period,
              std::uint8_t sample, std::uint8_t effect, std::uint8_t param) {
  ModCell& c = m.patterns[pat].cells[static_cast<std::size_t>(row) * m.channels + ch];
  c.period = period;
  c.sample = sample;
  c.effect = effect;
  c.param = param;
}

}  // namespace

int main() {
  const std::string dir = C2B_FIXTURE_DIR;

  // --- Fixture: 10k Reggae Dub (linear, tempo 150 via Fxx) ---
  {
    const Module m = parse_mod(read_bytes(dir + "/10k_reggae_dub.mod"));
    const Timeline tl = build_timeline(m);
    CHECK_MSG(tl.emitted_rows == 704, "rows=%ld", tl.emitted_rows);
    CHECK_MSG(tl.total_pulses == 42240, "pulses=%ld", tl.total_pulses);
    CHECK_MSG(tl.init_bpm == 150.0, "init_bpm=%g", tl.init_bpm);
    CHECK_MSG(tl.lines.size() == 44, "lines=%zu", tl.lines.size());
    CHECK_MSG(tl.notes.size() == 1426, "notes=%zu", tl.notes.size());
    // Lines fall on measure boundaries; notes are pulse-ordered.
    for (std::size_t i = 0; i < tl.lines.size(); ++i)
      CHECK(tl.lines[i] == static_cast<long>(i) * 960);
    bool ordered = true;
    for (std::size_t i = 1; i < tl.notes.size(); ++i)
      ordered &= tl.notes[i].pulse >= tl.notes[i - 1].pulse;
    CHECK(ordered);
    for (const NoteEvent& n : tl.notes) CHECK(n.period != 0 && n.sample != 0);
  }

  // --- Fixture: 8-bit Castle (linear, constant 125 BPM) ---
  {
    const Module m = parse_mod(read_bytes(dir + "/8bit_castle.mod"));
    const Timeline tl = build_timeline(m);
    CHECK_MSG(tl.emitted_rows == 1088, "rows=%ld", tl.emitted_rows);
    CHECK_MSG(tl.init_bpm == 125.0, "init_bpm=%g", tl.init_bpm);
    CHECK_MSG(tl.bpm_events.empty(), "bpm_events=%zu", tl.bpm_events.size());
    CHECK_MSG(tl.lines.size() == 68, "lines=%zu", tl.lines.size());
  }

  // --- Synthetic: Bxx backward jump + --max-loops unrolling ---
  {
    Module m = make_module(4, {0, 1}, 2);
    set_cell(m, 0, 0, 0, /*period=*/428, /*sample=*/1, 0, 0);
    set_cell(m, 1, 0, 0, 428, 1, 0, 0);
    set_cell(m, 1, 63, 0, 0, 0, /*Bxx=*/0xB, /*to order*/0x00);  // loop forever

    TimelineOptions o1;
    o1.max_loops = 1;
    Timeline t1 = build_timeline(m, o1);
    CHECK_MSG(t1.emitted_rows == 128, "rows=%ld", t1.emitted_rows);  // play once
    CHECK_MSG(t1.loops_played == 0, "loops=%d", t1.loops_played);

    TimelineOptions o2;
    o2.max_loops = 2;
    Timeline t2 = build_timeline(m, o2);
    CHECK_MSG(t2.emitted_rows == 256, "rows=%ld", t2.emitted_rows);  // play twice
    CHECK_MSG(t2.loops_played == 1, "loops=%d", t2.loops_played);
  }

  // --- Synthetic: Dxx pattern break shortens a pattern ---
  {
    Module m = make_module(4, {0, 1}, 2);
    set_cell(m, 0, 5, 0, 0, 0, /*Dxx=*/0xD, 0x00);  // break to row 0 of next order
    Timeline t = build_timeline(m);
    // order 0 plays rows 0..5 (6 rows), order 1 plays 64 rows, then ends.
    CHECK_MSG(t.emitted_rows == 70, "rows=%ld", t.emitted_rows);
  }

  // --- Synthetic: Fxx tempo change emits a bpm_event ---
  {
    Module m = make_module(4, {0}, 1);
    set_cell(m, 0, 8, 0, 0, 0, /*Fxx=*/0xF, 0x96);  // tempo 150 at row 8
    Timeline t = build_timeline(m);
    CHECK_MSG(t.init_bpm == 125.0, "init_bpm=%g", t.init_bpm);
    CHECK_MSG(t.bpm_events.size() == 1, "bpm_events=%zu", t.bpm_events.size());
    if (!t.bpm_events.empty()) {
      CHECK_MSG(t.bpm_events[0].pulse == 8 * 60, "pulse=%ld",
                t.bpm_events[0].pulse);
      CHECK_MSG(t.bpm_events[0].bpm == 150.0, "bpm=%g", t.bpm_events[0].bpm);
    }
  }

  REPORT_AND_RETURN();
}
