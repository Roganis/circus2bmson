// Exercise the keysound render path: descriptive, unique filenames; every
// timeline note bound to a written WAV.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/mod.hpp"
#include "circus2bmson/render.hpp"
#include "circus2bmson/timeline.hpp"

namespace {

using namespace circus2bmson;

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

bool is_riff_wav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  char hdr[4] = {0, 0, 0, 0};
  f.read(hdr, 4);
  f.seekg(0, std::ios::end);
  return f.good() && std::string(hdr, 4) == "RIFF" && f.tellg() > 44;
}

}  // namespace

int main() {
  const std::string dir = C2B_FIXTURE_DIR;
  const std::string out = std::string(C2B_TMP_DIR) + "/render";
  std::filesystem::create_directories(out);

  const std::vector<std::uint8_t> bytes = read_bytes(dir + "/10k_reggae_dub.mod");
  const Module mod = parse_mod(bytes);
  const Timeline tl = build_timeline(mod);

  const RenderResult rr = render_keysounds(bytes, mod, out, KeysoundNaming::Instrument);

  // Names are unique, descriptive, and back real WAV files.
  std::set<std::string> uniq(rr.keysound_names.begin(), rr.keysound_names.end());
  CHECK_MSG(uniq.size() == rr.keysound_names.size(), "names=%zu unique=%zu",
            rr.keysound_names.size(), uniq.size());
  CHECK(static_cast<long>(rr.keysound_names.size()) == rr.unique_keysounds);
  for (const std::string& n : rr.keysound_names) {
    CHECK_MSG(n.size() > 4 && n.substr(n.size() - 4) == ".wav", "name='%s'",
              n.c_str());
    CHECK_MSG(n[0] == 's', "instrument name should start with 's': '%s'",
              n.c_str());
    CHECK_MSG(is_riff_wav(out + "/" + n), "not a WAV: '%s'", n.c_str());
  }

  // Every timeline note resolves to a keysound; slice count matches notes.
  CHECK_MSG(rr.total_slices == static_cast<long>(tl.notes.size()),
            "slices=%ld notes=%zu", rr.total_slices, tl.notes.size());
  long missing = 0;
  for (const NoteEvent& n : tl.notes)
    if (!rr.note_to_keysound.count(note_key(n.order, n.row, n.channel))) ++missing;
  CHECK_MSG(missing == 0, "missing=%ld", missing);

  // Lane naming puts the channel first.
  const RenderResult rl = render_keysounds(bytes, mod, out, KeysoundNaming::Lane);
  for (const std::string& n : rl.keysound_names)
    CHECK_MSG(n.rfind("ch", 0) == 0, "lane name should start with 'ch': '%s'",
              n.c_str());

  REPORT_AND_RETURN();
}
