// End-to-end MIDI: parse + FluidSynth render -> valid bmson + keysounds.
// Skips (passes) when no SoundFont is installed, since MIDI needs one.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "circus2bmson/convert.hpp"

namespace {
std::string find_soundfont() {
  const char* paths[] = {"/usr/share/sounds/sf2/TimGM6mb.sf2",
                         "/usr/share/sounds/sf2/FluidR3_GM.sf2",
                         "/usr/share/sounds/sf2/default-GM.sf2"};
  for (const char* p : paths)
    if (std::filesystem::exists(p)) return p;
  return "";
}
}  // namespace

int main() {
  using namespace circus2bmson;
  const std::string sf = find_soundfont();
  if (sf.empty()) {
    std::printf("SKIP: no SoundFont installed\n");
    return 0;
  }

  const std::string dir = C2B_FIXTURE_DIR;
  const std::string out = std::string(C2B_TMP_DIR) + "/midi";
  ConvertOptions opts;
  opts.output_dir = out;
  opts.soundfont_path = sf;

  const ConvertResult r = convert_mod_file(dir + "/scale.mid", opts);
  CHECK_MSG(r.format == "midi", "format=%s", r.format.c_str());
  CHECK_MSG(r.note_count == 9, "notes=%zu", r.note_count);
  CHECK(r.init_bpm == 120.0);
  CHECK(r.keysound_count == 9);

  nlohmann::json d;
  std::ifstream(r.bmson_path) >> d;
  CHECK(d.at("info").at("resolution") == 480);
  std::size_t notes = 0, files_ok = 0;
  for (const auto& ch : d.at("sound_channels")) {
    const std::string name = ch.at("name");
    const auto p = std::filesystem::path(out) / name;
    if (std::filesystem::exists(p) && std::filesystem::file_size(p) > 44)
      ++files_ok;
    for (const auto& n : ch.at("notes")) {
      CHECK(n.at("x").get<int>() == 0);  // BGM lane
      ++notes;
    }
  }
  CHECK_MSG(notes == 9, "json notes=%zu", notes);
  CHECK_MSG(files_ok == d.at("sound_channels").size(), "keysound files=%zu/%zu",
            files_ok, d.at("sound_channels").size());

  REPORT_AND_RETURN();
}
