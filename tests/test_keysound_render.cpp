// Exercise the keysound render path: unique filenames per naming scheme, valid
// audio containers (WAV and OGG), and every note bound to a keysound.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/render.hpp"
#include "circus2bmson/score.hpp"

namespace {

using namespace circus2bmson;

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

std::string magic4(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  char hdr[4] = {0, 0, 0, 0};
  f.read(hdr, 4);
  return std::string(hdr, 4);
}

}  // namespace

int main() {
  const std::string dir = C2B_FIXTURE_DIR;
  const std::string out = std::string(C2B_TMP_DIR) + "/render";
  std::filesystem::create_directories(out);

  const std::vector<std::uint8_t> bytes = read_bytes(dir + "/10k_reggae_dub.mod");
  const Score score = read_score(bytes);

  // Default: channel naming, WAV.
  const RenderResult rc = render_keysounds(bytes, score, out);
  std::set<std::string> uniq(rc.keysound_names.begin(), rc.keysound_names.end());
  CHECK_MSG(uniq.size() == rc.keysound_names.size(), "names=%zu unique=%zu",
            rc.keysound_names.size(), uniq.size());
  CHECK(static_cast<long>(rc.keysound_names.size()) == rc.unique_keysounds);
  CHECK_MSG(rc.total_slices == static_cast<long>(score.notes.size()),
            "slices=%ld notes=%zu", rc.total_slices, score.notes.size());
  for (const std::string& n : rc.keysound_names) {
    CHECK_MSG(n.rfind("channel", 0) == 0, "name='%s'", n.c_str());
    CHECK_MSG(magic4(out + "/" + n) == "RIFF", "not a WAV: '%s'", n.c_str());
  }
  long unbound = 0;
  for (int id : rc.note_keysound)
    if (id < 0) ++unbound;
  CHECK_MSG(unbound == 0, "unbound notes=%ld", unbound);

  // Instrument / lane naming prefixes.
  const RenderResult ri =
      render_keysounds(bytes, score, out, KeysoundNaming::Instrument);
  for (const std::string& n : ri.keysound_names)
    CHECK_MSG(n[0] == 's', "instrument name: '%s'", n.c_str());
  const RenderResult rl =
      render_keysounds(bytes, score, out, KeysoundNaming::Lane);
  for (const std::string& n : rl.keysound_names)
    CHECK_MSG(n.rfind("ch", 0) == 0, "lane name: '%s'", n.c_str());

  // OGG output: valid container, same dedup count as WAV.
  const std::string oggdir = std::string(C2B_TMP_DIR) + "/render_ogg";
  std::filesystem::create_directories(oggdir);
  const RenderResult ro =
      render_keysounds(bytes, score, oggdir, KeysoundNaming::Channel,
                       /*volume_ramping=*/false, AudioFormat::Ogg);
  CHECK_MSG(ro.unique_keysounds == rc.unique_keysounds, "ogg=%ld wav=%ld",
            ro.unique_keysounds, rc.unique_keysounds);
  for (const std::string& n : ro.keysound_names) {
    CHECK_MSG(n.size() > 4 && n.substr(n.size() - 4) == ".ogg", "name='%s'",
              n.c_str());
    CHECK_MSG(magic4(oggdir + "/" + n) == "OggS", "not an OGG: '%s'", n.c_str());
  }

  REPORT_AND_RETURN();
}
