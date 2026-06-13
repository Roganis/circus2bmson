// game-music-emu spike: a hand-built VGM (SN76489, three square bursts on the
// first channel) loads via libgme, reports its voices, and scan_chip recovers
// the three note onsets by audio-domain detection. Skips when built without
// libgme.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.hpp"
#include "circus2bmson/chip.hpp"

namespace {
using std::uint32_t;
using std::uint8_t;

void le32(std::vector<uint8_t>& v, std::size_t off, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[off + i] = (x >> (8 * i)) & 0xFF;
}

// Minimal VGM 1.50: SN76489 at the NTSC clock, three ~0.1 s square notes on
// channel 0 separated by ~0.1 s gaps.
std::vector<uint8_t> make_vgm() {
  std::vector<uint8_t> v(0x40, 0);  // header
  v[0] = 'V'; v[1] = 'g'; v[2] = 'm'; v[3] = ' ';
  le32(v, 0x08, 0x150);     // version 1.50
  le32(v, 0x0C, 3579545);   // SN76489 clock (NTSC)
  le32(v, 0x34, 0x0C);      // data offset: 0x34 + 0x0C = 0x40

  auto psg = [&](uint8_t b) { v.push_back(0x50); v.push_back(b); };
  auto wait = [&](uint32_t n) {
    v.push_back(0x61);
    v.push_back(n & 0xFF);
    v.push_back((n >> 8) & 0xFF);
  };
  const uint32_t dur = 4410;  // 0.1 s at 44100
  const uint8_t freq_lo[3] = {0x0E, 0x0A, 0x06};
  uint32_t total = 0;
  for (int n = 0; n < 3; ++n) {
    psg(0x80 | (freq_lo[n] & 0x0F));  // ch0 tone latch, low 4 bits
    psg(0x02);                        // ch0 tone high bits (data byte)
    psg(0x90);                        // ch0 volume: attenuation 0 (note on)
    wait(dur);
    total += dur;
    psg(0x9F);                        // ch0 volume: attenuation F (note off)
    wait(dur);
    total += dur;
  }
  v.push_back(0x66);                          // end of sound data
  le32(v, 0x18, total);                       // total sample count
  le32(v, 0x04, static_cast<uint32_t>(v.size() - 4));  // EoF offset
  return v;
}
}  // namespace

int main() {
  using namespace circus2bmson;
  if (!chip_supported()) {
    std::printf("SKIP: built without libgme\n");
    return 0;
  }

  const ChipScan s = scan_chip(make_vgm());
  std::printf("  type=%s system=%s voices=%zu frames=%ld\n", s.type.c_str(),
              s.system.c_str(), s.voices.size(), s.total_frames);
  CHECK_MSG(!s.type.empty(), "type='%s'", s.type.c_str());
  CHECK(s.voices.size() >= 1);
  CHECK(s.total_frames > 0);

  long onsets = 0;
  for (const ChipVoice& v : s.voices) onsets += static_cast<long>(v.onset_frames.size());
  CHECK_MSG(onsets == 3, "total onsets=%ld want 3", onsets);

  REPORT_AND_RETURN();
}
