// Furnace backend: a hand-built DefleMask module (v24, SMS/SN76489) is rendered
// per-channel by the Furnace binary, the onsets are recovered from the audio,
// and the converter emits a valid bmson with keysound files. Skips when Furnace
// is not installed.
//
// The fixture is built here rather than committed so the repo carries no
// third-party module, and so the exact DMF layout is spelled out in code -- the
// same trick test_chip.cpp uses for its VGM.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "check.hpp"
#include "circus2bmson/convert.hpp"
#include "circus2bmson/furnace.hpp"

namespace {
using std::uint8_t;

constexpr int kRows = 16;    // rows per pattern
constexpr int kMatrix = 1;   // pattern-matrix rows (order list length)
constexpr int kChans = 4;    // SMS: 3 square + 1 noise
constexpr int kFxCols = 1;

void u8v(std::vector<uint8_t>& v, int x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}
void i16v(std::vector<uint8_t>& v, int x) {  // little-endian signed short
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void u32v(std::vector<uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void pstr(std::vector<uint8_t>& v, const char* s) {  // length-prefixed string
  const std::string t(s);
  u8v(v, static_cast<int>(t.size()));
  for (char c : t) v.push_back(static_cast<uint8_t>(c));
}

// A DMF is a zlib stream. Four notes on channel 0, each followed by a NOTE OFF
// so the envelope dips between them: the onset detector keys on amplitude, and
// a chip square wave sliding from pitch to pitch with no gap is exactly the
// legato case it cannot see (onset.hpp). Real modules do hit that limit -- which
// is why parsing the pattern data for an exact score is the next step.
std::vector<uint8_t> make_dmf() {
  std::vector<uint8_t> b;
  const char* magic = ".DelekDefleMask.";
  for (int i = 0; i < 16; ++i) b.push_back(static_cast<uint8_t>(magic[i]));
  u8v(b, 0x18);  // format version 24
  u8v(b, 0x03);  // system: SEGA Master System (4 channels)
  pstr(b, "c2b test");
  pstr(b, "circus2bmson");
  u8v(b, 4);     // highlight A
  u8v(b, 4);     // highlight B
  u8v(b, 0);     // time base -> ticks per row multiply by (timeBase + 1) = 1
  u8v(b, 6);     // tick time 1 (speed A)
  u8v(b, 6);     // tick time 2 (speed B)
  u8v(b, 1);     // frames mode: 1 = NTSC -> 60 Hz
  u8v(b, 0);     // using custom Hz: no
  b.push_back('0'); b.push_back('0'); b.push_back('0');  // custom Hz: 3 ASCII digits
  u32v(b, kRows);   // rows per pattern (4 bytes since version > 0x17)
  u8v(b, kMatrix);  // rows in the pattern matrix

  // PATTERN MATRIX, channel-major. Pattern names only exist for version > 0x18.
  for (int c = 0; c < kChans; ++c)
    for (int m = 0; m < kMatrix; ++m) u8v(b, 0);

  // INSTRUMENTS: one STD instrument with four empty macros (volume, arpeggio,
  // duty, wavetable). The arpeggio macro's mode byte is always present, even
  // when the macro is empty -- verified against Furnace, which rejects the file
  // otherwise.
  u8v(b, 1);
  pstr(b, "square");
  u8v(b, 0);  // mode 0 = STD
  u8v(b, 0);  // volume macro: size 0
  u8v(b, 0);  // arpeggio macro: size 0
  u8v(b, 0);  //   ... followed by ARP_MACRO_MODE regardless
  u8v(b, 0);  // duty/noise macro: size 0
  u8v(b, 0);  // wavetable macro: size 0

  u8v(b, 0);  // WAVETABLES: none

  // PATTERNS. Note 12 is C (not 0); 100 is NOTE OFF; -1 means "empty".
  const int note_row[4] = {0, 4, 8, 12};
  const int note_val[4] = {12, 4, 7, 12};  // C-3 E-3 G-3 C-4
  const int note_oct[4] = {3, 3, 3, 4};
  const int off_row[4] = {2, 6, 10, 14};
  for (int c = 0; c < kChans; ++c) {
    u8v(b, kFxCols);  // effect column count is per channel
    for (int m = 0; m < kMatrix; ++m) {
      for (int row = 0; row < kRows; ++row) {
        int k = -1, o = -1;
        for (int i = 0; i < 4; ++i)
          if (note_row[i] == row) k = i;
        for (int i = 0; i < 4; ++i)
          if (off_row[i] == row) o = i;

        if (c == 0 && k >= 0) {
          i16v(b, note_val[k]);
          i16v(b, note_oct[k]);
          i16v(b, 15);  // volume
        } else if (c == 0 && o >= 0) {
          i16v(b, 100);  // NOTE OFF (octave ignored)
          i16v(b, 0);
          i16v(b, -1);
        } else {
          i16v(b, 0);
          i16v(b, 0);
          i16v(b, -1);
        }
        for (int f = 0; f < kFxCols; ++f) {
          i16v(b, -1);  // effect code
          i16v(b, -1);  // effect value
        }
        i16v(b, (c == 0 && k >= 0) ? 0 : -1);  // instrument
      }
    }
  }

  u8v(b, 0);  // PCM SAMPLES: none

  uLongf cap = compressBound(static_cast<uLong>(b.size()));
  std::vector<uint8_t> out(cap);
  if (compress2(out.data(), &cap, b.data(), static_cast<uLong>(b.size()),
                Z_DEFAULT_COMPRESSION) != Z_OK)
    return {};
  out.resize(cap);
  return out;
}
}  // namespace

int main() {
  using namespace circus2bmson;
  namespace fs = std::filesystem;

  try {
    resolve_furnace("");
  } catch (const std::exception& e) {
    // CI sets C2B_REQUIRE_FURNACE so a missing binary is a failure rather than
    // a silent skip -- otherwise the backend could rot untested and green.
    if (const char* req = std::getenv("C2B_REQUIRE_FURNACE")) {
      if (*req && std::string(req) != "0") {
        std::printf("FAIL: C2B_REQUIRE_FURNACE is set but %s\n", e.what());
        return 1;
      }
    }
    std::printf("SKIP: Furnace not installed (see furnace.hpp)\n");
    return 0;
  }

  CHECK(furnace_handles_extension(".dmf"));
  CHECK(furnace_handles_extension("FUR"));
  CHECK(!furnace_handles_extension(".mod"));

  const fs::path dir(C2B_TMP_DIR);
  fs::create_directories(dir);
  const fs::path input = dir / "tiny.dmf";
  const std::vector<uint8_t> dmf = make_dmf();
  CHECK(!dmf.empty());
  {
    std::ofstream f(input, std::ios::binary);
    f.write(reinterpret_cast<const char*>(dmf.data()),
            static_cast<std::streamsize>(dmf.size()));
  }

  ConvertOptions opts;
  opts.output_dir = (dir / "tiny_out").string();
  const ConvertResult r = convert_mod_file(input.string(), opts);

  std::printf("  format=%s channels=%d notes=%zu keysounds=%ld  %.2f s\n",
              r.format.c_str(), r.channels, r.note_count, r.keysound_count,
              r.total_seconds);

  CHECK_MSG(r.format == "dmf", "format='%s'", r.format.c_str());
  CHECK_MSG(r.channels == kChans, "channels=%d want %d", r.channels, kChans);
  // Four note-ons, each its own keysound (chip stems never repeat byte-exactly).
  CHECK_MSG(r.note_count == 4, "notes=%zu want 4", r.note_count);
  CHECK(r.keysound_count == 4);
  CHECK(r.audio_rendered);

  // 16 rows at speed 6, 60 Hz -> 16 * 6 / 60 = 1.6 s.
  CHECK_MSG(r.total_seconds > 1.5 && r.total_seconds < 1.7, "%.3f s",
            r.total_seconds);

  // The bmson parses, puts every note on the BGM lane, and each keysound it
  // names was actually written.
  std::ifstream in(r.bmson_path);
  CHECK(in.good());
  const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
  CHECK(!doc.is_discarded());
  if (!doc.is_discarded()) {
    std::size_t notes = 0;
    for (const auto& ch : doc["sound_channels"]) {
      const std::string name = ch["name"];
      CHECK_MSG(fs::exists(fs::path(opts.output_dir) / name),
                "missing keysound %s", name.c_str());
      for (const auto& n : ch["notes"]) {
        CHECK(n["x"] == 0);  // BGM lane
        ++notes;
      }
    }
    CHECK_MSG(notes == 4, "bmson notes=%zu want 4", notes);
  }

  REPORT_AND_RETURN();
}
