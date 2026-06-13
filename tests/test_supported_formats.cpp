// supported_input_extensions(): sourced from the linked libopenmpt plus MIDI,
// returned sorted and de-duplicated, and including the staple formats.
#include <algorithm>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/convert.hpp"

int main() {
  const std::vector<std::string> e = circus2bmson::supported_input_extensions();
  CHECK(!e.empty());
  CHECK(std::is_sorted(e.begin(), e.end()));
  CHECK(std::adjacent_find(e.begin(), e.end()) == e.end());  // de-duplicated

  auto has = [&](const char* x) {
    return std::find(e.begin(), e.end(), std::string(x)) != e.end();
  };
  for (const char* x : {"mod", "xm", "s3m", "it", "mid", "midi"})
    CHECK_MSG(has(x), "missing extension '%s'", x);

  REPORT_AND_RETURN();
}
