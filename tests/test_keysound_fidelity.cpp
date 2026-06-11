// End-to-end: the rendered keysounds, placed back at their onsets, must
// reconstruct libopenmpt's full mix -- for every supported format family.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/render.hpp"
#include "circus2bmson/score.hpp"

namespace {

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

void check_fixture(const std::string& path) {
  using namespace circus2bmson;
  const std::vector<std::uint8_t> bytes = read_bytes(path);
  const Score score = read_score(bytes);
  const double db = reconstruct_residual_db(bytes, score);
  std::printf("  %-50s residual = %.1f dB (%zu notes, %s)\n", path.c_str(), db,
              score.notes.size(), score.format.c_str());
  CHECK_MSG(db <= -60.0, "%s residual=%.1f dB", path.c_str(), db);
}

}  // namespace

int main() {
  const std::string dir = C2B_FIXTURE_DIR;
  check_fixture(dir + "/10k_reggae_dub.mod");
  check_fixture(dir + "/8bit_castle.mod");
  check_fixture(dir + "/neurosys.xm");
  check_fixture(dir + "/chip_ultimatum.it");
  REPORT_AND_RETURN();
}
