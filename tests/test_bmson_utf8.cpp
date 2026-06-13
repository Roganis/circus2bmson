// Regression: a non-UTF-8 title (e.g. a Shift-JIS MIDI track name) must not
// make bmson emission throw -- invalid bytes are replaced, not fatal.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "circus2bmson/bmson.hpp"
#include "circus2bmson/score.hpp"

int main() {
  using namespace circus2bmson;
  Score sc;
  sc.title = std::string("\x82\xb1\x82\xea");  // invalid UTF-8 (Shift-JIS bytes)
  sc.init_bpm = 120.0;
  sc.resolution = 240;

  std::string doc;
  bool threw = false;
  try {
    doc = build_bmson(sc, {});
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);

  bool parsed = true;
  try {
    auto j = nlohmann::json::parse(doc);
    (void)j;
  } catch (...) {
    parsed = false;
  }
  CHECK(parsed);

  REPORT_AND_RETURN();
}
