// Audio-domain onset detection: square bursts at known frames are recovered;
// a gapless sustain registers a single onset (the documented legato limit);
// silence yields none.
#include <cstdlib>
#include <vector>

#include "check.hpp"
#include "circus2bmson/onset.hpp"

namespace {
using namespace circus2bmson;

void square(std::vector<float>& buf, long frames, float amp) {
  for (long f = 0; f < frames; ++f) {
    const float s = ((f / 16) % 2) ? amp : -amp;  // ~1.4 kHz square at 44100
    buf.push_back(s);
    buf.push_back(s);
  }
}
void silence(std::vector<float>& buf, long frames) {
  buf.insert(buf.end(), static_cast<std::size_t>(frames) * 2, 0.0f);
}
}  // namespace

int main() {
  const int rate = 44100;
  const long note = 4410, gap = 4410;  // 0.1 s each

  // Three notes separated by silence -> three onsets near 0, 8820, 17640.
  std::vector<float> a;
  square(a, note, 0.3f);
  silence(a, gap);
  square(a, note, 0.3f);
  silence(a, gap);
  square(a, note, 0.3f);
  const std::vector<long> on =
      detect_onsets(a.data(), static_cast<long>(a.size() / 2), rate);
  CHECK_MSG(on.size() == 3, "onsets=%zu want 3", on.size());
  if (on.size() == 3) {
    const long want[3] = {0, note + gap, 2 * (note + gap)};
    for (int i = 0; i < 3; ++i)
      CHECK_MSG(std::labs(on[i] - want[i]) <= 256, "onset %d at %ld want ~%ld", i,
                on[i], want[i]);
  }

  // Gapless sustain -> a single attack.
  std::vector<float> b;
  square(b, note * 4, 0.3f);
  CHECK_MSG(detect_onsets(b.data(), static_cast<long>(b.size() / 2), rate).size() == 1,
            "sustain should be one onset");

  // Pure silence -> nothing.
  std::vector<float> c;
  silence(c, note);
  CHECK(detect_onsets(c.data(), static_cast<long>(c.size() / 2), rate).empty());

  REPORT_AND_RETURN();
}
