#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "circus2bmson/mod.hpp"

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

  {
    const Module m = parse_mod(read_bytes(dir + "/10k_reggae_dub.mod"));
    CHECK_MSG(m.title == "10k Reggae Dub", "title='%s'", m.title.c_str());
    CHECK_MSG(m.channels == 4, "channels=%d", m.channels);
    CHECK_MSG(m.format_tag == "M.K.", "tag='%s'", m.format_tag.c_str());
    CHECK_MSG(m.samples.size() == 31, "samples=%zu", m.samples.size());
    CHECK_MSG(m.song_length == 11, "song_length=%d", m.song_length);
    CHECK(!m.patterns.empty());
    // Order list replays patterns: [0,1,1,5,5,4,4,3,3,2,2].
    CHECK(m.order[0] == 0 && m.order[1] == 1 && m.order[2] == 1);
  }

  {
    const Module m = parse_mod(read_bytes(dir + "/8bit_castle.mod"));
    CHECK_MSG(m.title == "8-bit Castle", "title='%s'", m.title.c_str());
    CHECK_MSG(m.channels == 4, "channels=%d", m.channels);
    CHECK_MSG(m.format_tag == "4CHN", "tag='%s'", m.format_tag.c_str());
    CHECK_MSG(m.song_length == 17, "song_length=%d", m.song_length);
    // At least one sample should carry real PCM length.
    bool any_len = false;
    for (const ModSample& s : m.samples) any_len |= (s.length > 0);
    CHECK(any_len);
  }

  // Unsupported signature must throw rather than misparse.
  {
    std::vector<std::uint8_t> junk(2000, 0);
    junk[1080] = 'X'; junk[1081] = 'X'; junk[1082] = 'X'; junk[1083] = 'X';
    bool threw = false;
    try {
      parse_mod(junk);
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }

  REPORT_AND_RETURN();
}
