// Cross-check the timeline's implied real-time length against libopenmpt's
// own duration, and validate the emitted bmson document.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <libopenmpt/libopenmpt.hpp>
#include <nlohmann/json.hpp>

#include "check.hpp"
#include "circus2bmson/convert.hpp"

namespace {

std::vector<char> read_chars(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
}

void check_fixture(const std::string& mod_path, const std::string& out_dir) {
  using namespace circus2bmson;

  // libopenmpt's own single-play duration.
  const std::vector<char> bytes = read_chars(mod_path);
  openmpt::module ref(bytes);
  const double ref_seconds = ref.get_duration_seconds();

  ConvertOptions opts;
  opts.output_dir = out_dir;
  const ConvertResult r = convert_mod_file(mod_path, opts);

  // Our flattened timeline should imply (nearly) the same real-time length.
  const double tol = std::max(1.0, 0.01 * ref_seconds);
  CHECK_MSG(std::fabs(r.total_seconds - ref_seconds) <= tol,
            "%s: ours=%.3fs libopenmpt=%.3fs", mod_path.c_str(),
            r.total_seconds, ref_seconds);

  // The emitted bmson must be valid JSON with notes only on the BGM lane.
  std::ifstream jf(r.bmson_path, std::ios::binary);
  CHECK(static_cast<bool>(jf));
  nlohmann::json doc;
  bool parsed = true;
  try {
    jf >> doc;
  } catch (const std::exception&) {
    parsed = false;
  }
  CHECK_MSG(parsed, "bmson did not parse: %s", r.bmson_path.c_str());
  if (!parsed) return;

  CHECK(doc.at("version") == "1.0.0");
  CHECK(doc.at("info").at("resolution") == 240);
  CHECK(doc.at("info").at("init_bpm").get<double>() == r.init_bpm);

  std::size_t notes = 0;
  long last_line = -1;
  bool lines_sorted = true;
  for (const auto& l : doc.at("lines"))
    if (l.at("y").get<long>() < last_line) lines_sorted = false;
    else last_line = l.at("y").get<long>();
  CHECK(lines_sorted);

  for (const auto& ch : doc.at("sound_channels"))
    for (const auto& n : ch.at("notes")) {
      CHECK(n.at("x").get<int>() == 0);  // BGM lane only
      ++notes;
    }
  CHECK_MSG(notes == r.note_count, "json notes=%zu result=%zu", notes,
            r.note_count);
}

}  // namespace

int main() {
  const std::string dir = C2B_FIXTURE_DIR;
  const std::string out = C2B_TMP_DIR;
  check_fixture(dir + "/10k_reggae_dub.mod", out);
  check_fixture(dir + "/8bit_castle.mod", out);
  REPORT_AND_RETURN();
}
