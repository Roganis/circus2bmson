// Oracle cross-check: the generic libopenmpt-driven score (score.cpp) must
// agree with the independent MOD parser + sequencer (mod.cpp/timeline.cpp) on
// real MOD fixtures -- two implementations, one truth.
#include <cmath>
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

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

void check_fixture(const std::string& path) {
  using namespace circus2bmson;
  const std::vector<std::uint8_t> bytes = read_bytes(path);

  const Module mod = parse_mod(bytes);
  const Timeline tl = build_timeline(mod);
  const Score sc = read_score(bytes);

  std::printf("  %s: rows %ld/%zu notes %zu/%zu bpm_evts %zu/%zu\n",
              path.c_str(), tl.emitted_rows, sc.rows.size(), tl.notes.size(),
              sc.notes.size(), tl.bpm_events.size(), sc.bpm_events.size());

  CHECK_MSG(static_cast<long>(sc.rows.size()) == tl.emitted_rows,
            "rows score=%zu timeline=%ld", sc.rows.size(), tl.emitted_rows);
  CHECK(sc.total_pulses == tl.total_pulses);
  CHECK_MSG(std::fabs(sc.init_bpm - tl.init_bpm) < 1e-9, "init %g vs %g",
            sc.init_bpm, tl.init_bpm);
  CHECK(sc.coarse_rows == 0);
  CHECK(!sc.truncated);

  // Played (order, row) sequence must match the sequencer's.
  const std::size_t nrows =
      std::min<std::size_t>(sc.rows.size(), static_cast<std::size_t>(tl.emitted_rows));
  // Timeline does not retain per-row order/row; rebuild from its notes instead:
  // compare the note streams, which carry (pulse, channel) in play order.
  CHECK_MSG(sc.notes.size() == tl.notes.size(), "notes %zu vs %zu",
            sc.notes.size(), tl.notes.size());
  const std::size_t nn = std::min(sc.notes.size(), tl.notes.size());
  int mismatches = 0;
  for (std::size_t i = 0; i < nn; ++i) {
    if (sc.notes[i].pulse != tl.notes[i].pulse ||
        sc.notes[i].channel != tl.notes[i].channel)
      ++mismatches;
  }
  CHECK_MSG(mismatches == 0, "note stream mismatches=%d", mismatches);

  // BPM events must agree in position and value.
  CHECK_MSG(sc.bpm_events.size() == tl.bpm_events.size(), "bpm %zu vs %zu",
            sc.bpm_events.size(), tl.bpm_events.size());
  const std::size_t nb = std::min(sc.bpm_events.size(), tl.bpm_events.size());
  for (std::size_t i = 0; i < nb; ++i) {
    CHECK(sc.bpm_events[i].pulse == tl.bpm_events[i].pulse);
    CHECK(std::fabs(sc.bpm_events[i].bpm - tl.bpm_events[i].bpm) < 1e-9);
  }

  (void)nrows;
}

}  // namespace

int main() {
  const std::string dir = C2B_FIXTURE_DIR;
  check_fixture(dir + "/10k_reggae_dub.mod");
  check_fixture(dir + "/8bit_castle.mod");
  REPORT_AND_RETURN();
}
