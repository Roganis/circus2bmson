#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/midi.hpp"

namespace {
std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
  using namespace circus2bmson;
  const std::string dir = C2B_FIXTURE_DIR;
  const MidiSong s = parse_smf(read_bytes(dir + "/scale.mid"));

  CHECK_MSG(s.division == 480, "division=%d", s.division);
  CHECK_MSG(s.title == "scale", "title='%s'", s.title.c_str());
  // chord (3) + drums (2) + melody (4) = 9 notes.
  CHECK_MSG(s.notes.size() == 9, "notes=%zu", s.notes.size());
  CHECK_MSG(s.tempos.size() == 1 && s.tempos[0].usec_per_qn == 500000,
            "tempos=%zu", s.tempos.size());

  int drums = 0, melodic = 0;
  bool durations_ok = true;
  for (const MidiNote& n : s.notes) {
    if (n.drum) ++drums; else ++melodic;
    durations_ok &= (n.tick_off > n.tick_on);
    CHECK(n.velocity >= 1 && n.velocity <= 127);
  }
  CHECK_MSG(drums == 2, "drums=%d", drums);
  CHECK_MSG(melodic == 7, "melodic=%d", melodic);
  CHECK(durations_ok);

  // First three notes are the tick-0 chord on channel 0, program 0.
  CHECK(s.notes[0].tick_on == 0 && s.notes[0].channel == 0 &&
        s.notes[0].program == 0);
  CHECK(s.end_tick == 1440);
  CHECK(!s.bar_ticks.empty() && s.bar_ticks[0] == 0);

  REPORT_AND_RETURN();
}
