// Sub-row precision: the note-delay effect (EDx on MOD/XM, SDx on S3M/IT)
// shifts a note part-way into its row, both in pulses (chart) and in its slice
// onset (audio). Verified two ways: a synthetic Module drives the MOD oracle
// deterministically, and the real fixtures are scanned so that, when they use
// note delays, score.cpp's libopenmpt-side detection is exercised here and
// cross-checked against the oracle by the score-vs-timeline test.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/mod.hpp"
#include "circus2bmson/score.hpp"
#include "circus2bmson/timeline.hpp"

namespace {
using namespace circus2bmson;

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

// A 4-channel module: one pattern of `rows` rows filled with `cells`.
Module make_module(int rows, std::vector<ModCell> cells) {
  Module m;
  m.channels = 4;
  m.song_length = 1;
  m.order.assign(128, 0);
  m.samples.resize(31);
  ModPattern pat;
  pat.rows = rows;
  pat.channels = 4;
  pat.cells = std::move(cells);
  m.patterns.push_back(std::move(pat));
  return m;
}

long pulse_of(const Timeline& tl, int row, int ch) {
  for (const auto& n : tl.notes)
    if (n.row == row && n.channel == ch) return n.pulse;
  return -1;
}

// A minimal but valid 31-sample "M.K." MOD: one pattern, one short sample, a
// C-2 on channel 0 row 0 with note-delay ED3, then a plain C-2 on row 1. Used
// to drive score.cpp's libopenmpt path (which derives the delay from the
// formatted effect column) without committing a binary fixture.
std::vector<std::uint8_t> make_mod_ed3() {
  std::vector<std::uint8_t> b(1084, 0);  // header through the signature
  const int s1 = 20;                     // sample 1 header
  const int words = 32;                  // 64 bytes of PCM
  b[s1 + 22] = (words >> 8) & 0xFF;      // length (big-endian words)
  b[s1 + 23] = words & 0xFF;
  b[s1 + 25] = 64;                       // volume
  b[s1 + 29] = 1;                        // loop length 1 word == no loop
  b[950] = 1;                            // song length: one order
  b[951] = 0x7F;                         // restart
  b[952] = 0;                            // order[0] -> pattern 0
  b[1080] = 'M'; b[1081] = '.'; b[1082] = 'K'; b[1083] = '.';

  std::vector<std::uint8_t> pat(1024, 0);  // 64 rows x 4 ch x 4 bytes
  auto cell = [&](int row, int ch, std::uint8_t b0, std::uint8_t b1,
                  std::uint8_t b2, std::uint8_t b3) {
    const int o = (row * 4 + ch) * 4;
    pat[o] = b0; pat[o + 1] = b1; pat[o + 2] = b2; pat[o + 3] = b3;
  };
  // period 428 (0x1AC) = C-2, sample 1. byte0=sample_hi|period_hi,
  // byte1=period_lo, byte2=sample_lo|effect, byte3=param.
  cell(0, 0, 0x01, 0xAC, 0x1E, 0xD3);  // C-2 s1  ED3 (delay 3 ticks)
  cell(1, 0, 0x01, 0xAC, 0x10, 0x00);  // C-2 s1  (plain)
  b.insert(b.end(), pat.begin(), pat.end());

  for (int i = 0; i < 64; ++i)  // square-wave PCM, 8-bit signed
    b.push_back(i < 32 ? 0x40 : 0xC0);
  return b;
}
}  // namespace

int main() {
  // --- Synthetic oracle: known delays -> known sub-row pulses (60/row). ---
  // Row 0 ch0: note + ED3, default speed 6 -> round(3/6*60) = 30.
  // Row 1 ch0: plain note                  -> 60.
  // Row 2 ch0: note + ED6, speed 6 -> delay clamped to 5 -> round(5/6*60) = 50.
  {
    std::vector<ModCell> cells(3 * 4);
    cells[0 * 4 + 0] = {1, 428, 0xE, 0xD3};
    cells[1 * 4 + 0] = {1, 428, 0x0, 0x00};
    cells[2 * 4 + 0] = {1, 428, 0xE, 0xD6};
    const Timeline tl = build_timeline(make_module(3, cells));
    CHECK_MSG(pulse_of(tl, 0, 0) == 30, "ED3 pulse=%ld want 30", pulse_of(tl, 0, 0));
    CHECK_MSG(pulse_of(tl, 1, 0) == 60, "plain pulse=%ld want 60", pulse_of(tl, 1, 0));
    // Row 2 base 120 + clamped offset 50.
    CHECK_MSG(pulse_of(tl, 2, 0) == 170, "ED6 pulse=%ld want 170", pulse_of(tl, 2, 0));
  }

  // Speed scales the fraction: F03 (speed 3) + ED1 -> round(1/3*60) = 20.
  {
    std::vector<ModCell> cells(2 * 4);
    cells[0 * 4 + 0] = {0, 0, 0xF, 0x03};       // set speed 3
    cells[0 * 4 + 1] = {1, 428, 0xE, 0xD1};     // note + ED1, same row
    const Timeline tl = build_timeline(make_module(2, cells));
    CHECK_MSG(pulse_of(tl, 0, 1) == 20, "speed3 ED1 pulse=%ld want 20",
              pulse_of(tl, 0, 1));
  }

  // --- score.cpp (libopenmpt path): the synthetic MOD's ED3 note must land at
  // pulse 30 (3/6 of a 60-pulse row), the plain note at 60. This is the real
  // production path and the only place the effect-column parsing is exercised.
  {
    const Score sc = read_score(make_mod_ed3());
    CHECK_MSG(sc.notes.size() == 2, "score notes=%zu want 2", sc.notes.size());
    if (sc.notes.size() == 2) {
      CHECK_MSG(sc.notes[0].pulse == 30, "ED3 score pulse=%ld want 30",
                sc.notes[0].pulse);
      CHECK_MSG(sc.notes[1].pulse == 60, "plain score pulse=%ld want 60",
                sc.notes[1].pulse);
      // The delayed note's slice onset is pushed ~halfway into row 0, not at 0.
      CHECK_MSG(sc.notes[0].frame > sc.rows[0].frame,
                "ED3 onset frame=%ld not after row start=%ld",
                sc.notes[0].frame, sc.rows[0].frame);
    }
  }

  // --- Real fixtures: count played note-delay cells; when present, score.cpp
  // must place those notes off the row grid (and score_vs_timeline confirms it
  // matches the oracle byte-for-byte). ---
  const std::string dir = C2B_FIXTURE_DIR;
  for (const char* f : {"/10k_reggae_dub.mod", "/8bit_castle.mod"}) {
    const Module m = parse_mod(read_bytes(dir + f));
    long delays = 0;
    for (int o = 0; o < m.song_length; ++o) {
      const ModPattern& pat = m.patterns[m.order[o]];
      for (const ModCell& c : pat.cells)
        if (c.period != 0 && c.effect == 0xE && (c.param >> 4) == 0xD &&
            (c.param & 0x0F) != 0)
          ++delays;
    }
    const Score sc = read_score(read_bytes(dir + f));
    long subrow = 0;
    for (const ScoreNote& n : sc.notes)
      if (n.pulse % sc.pulses_per_row != 0) ++subrow;
    std::printf("  %s: played note-delay cells=%ld, sub-row notes (score)=%ld\n",
                f, delays, subrow);
    if (delays > 0) CHECK_MSG(subrow > 0, "delays=%ld but no sub-row notes", delays);
  }

  REPORT_AND_RETURN();
}
