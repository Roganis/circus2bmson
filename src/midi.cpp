#include "circus2bmson/midi.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>

namespace circus2bmson {
namespace {

// Bounded byte cursor over the file.
struct Reader {
  const std::uint8_t* p;
  const std::uint8_t* end;
  void need(std::size_t n) const {
    if (static_cast<std::size_t>(end - p) < n)
      throw std::runtime_error("truncated MIDI file");
  }
  std::uint8_t u8() {
    need(1);
    return *p++;
  }
  std::uint32_t u32() {
    need(4);
    std::uint32_t v = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                      (std::uint32_t(p[2]) << 8) | p[3];
    p += 4;
    return v;
  }
  std::uint16_t u16() {
    need(2);
    std::uint16_t v = static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    p += 2;
    return v;
  }
  // Variable-length quantity.
  std::uint32_t vlq() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      std::uint8_t b = u8();
      v = (v << 7) | (b & 0x7f);
      if (!(b & 0x80)) return v;
    }
    return v;
  }
};

}  // namespace

MidiSong parse_smf(const std::vector<std::uint8_t>& bytes) {
  Reader r{bytes.data(), bytes.data() + bytes.size()};
  if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'h' || r.u8() != 'd')
    throw std::runtime_error("not a Standard MIDI File (missing MThd)");
  const std::uint32_t hlen = r.u32();
  const std::uint16_t format = r.u16();
  const std::uint16_t ntrks = r.u16();
  const std::int16_t division = static_cast<std::int16_t>(r.u16());
  if (division <= 0)
    throw std::runtime_error("SMPTE-timed MIDI files are not supported");
  if (format > 1)
    throw std::runtime_error("unsupported MIDI format " + std::to_string(format));
  r.p += (hlen > 6 ? hlen - 6 : 0);  // skip any extra header bytes

  MidiSong s;
  s.division = division;
  std::vector<std::pair<long, int>> timesigs;  // (tick, ticks_per_measure)

  for (int t = 0; t < ntrks; ++t) {
    if (r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'r' || r.u8() != 'k')
      throw std::runtime_error("expected MTrk chunk");
    const std::uint32_t tlen = r.u32();
    const std::uint8_t* tend = r.p + tlen;
    if (tend > r.end) throw std::runtime_error("truncated MTrk chunk");

    long tick = 0;
    std::uint8_t status = 0;
    int program[16] = {0};
    // Per-channel continuous controllers, seeded with the GM power-on defaults
    // so notes before any CC still snapshot sensible values.
    int ctrl_vol[16];   // CC7  channel volume
    int ctrl_expr[16];  // CC11 expression
    int ctrl_pan[16];   // CC10 pan
    for (int i = 0; i < 16; ++i) {
      ctrl_vol[i] = 100;
      ctrl_expr[i] = 127;
      ctrl_pan[i] = 64;
    }
    std::map<int, std::size_t> open;  // channel*128+key -> index of open note

    auto close_note = [&](int k, long off_tick) {
      auto it = open.find(k);
      if (it == open.end()) return;
      s.notes[it->second].tick_off = off_tick;
      open.erase(it);
    };

    while (r.p < tend) {
      tick += r.vlq();
      std::uint8_t b = r.u8();
      if (b & 0x80) {
        status = b;
      } else {
        r.p--;  // running status: reuse previous status, b is first data byte
      }
      const std::uint8_t hi = status & 0xF0;
      const int ch = status & 0x0F;

      if (status == 0xFF) {  // meta
        const std::uint8_t type = r.u8();
        const std::uint32_t len = r.vlq();
        const std::uint8_t* data = r.p;
        r.need(len);
        r.p += len;
        if (type == 0x51 && len == 3) {
          const int usec = (data[0] << 16) | (data[1] << 8) | data[2];
          s.tempos.push_back({tick, usec});
        } else if (type == 0x58 && len >= 2) {
          const int num = data[0];
          const int denom_pow = data[1];  // denominator = 2^denom_pow
          const long tpm = static_cast<long>(num) * 4 * s.division /
                            (1 << denom_pow);
          timesigs.push_back({tick, tpm > 0 ? static_cast<int>(tpm) : s.division * 4});
        } else if (type == 0x03 && s.title.empty()) {
          s.title.assign(reinterpret_cast<const char*>(data), len);
        }
      } else if (status == 0xF0 || status == 0xF7) {  // sysex
        const std::uint32_t len = r.vlq();
        r.need(len);
        r.p += len;
      } else if (hi == 0x90) {  // note on
        const int key = r.u8(), vel = r.u8();
        const int k = ch * 128 + key;
        if (vel == 0) {
          close_note(k, tick);
        } else {
          close_note(k, tick);  // retrigger
          MidiNote n;
          n.tick_on = tick;
          n.channel = ch;
          n.key = key;
          n.velocity = vel;
          n.program = program[ch];
          n.drum = (ch == 9);
          n.volume = ctrl_vol[ch];
          n.expression = ctrl_expr[ch];
          n.pan = ctrl_pan[ch];
          open[k] = s.notes.size();
          s.notes.push_back(n);
        }
      } else if (hi == 0x80) {  // note off
        const int key = r.u8();
        r.u8();  // velocity
        close_note(ch * 128 + key, tick);
      } else if (hi == 0xC0 || hi == 0xD0) {  // program / channel pressure: 1 byte
        const std::uint8_t v = r.u8();
        if (hi == 0xC0) program[ch] = v;
      } else if (hi == 0xB0) {  // control change
        const int cc = r.u8();
        const int val = r.u8();
        if (cc == 7) ctrl_vol[ch] = val;
        else if (cc == 11) ctrl_expr[ch] = val;
        else if (cc == 10) ctrl_pan[ch] = val;
      } else if (hi == 0xA0 || hi == 0xE0) {  // aftertouch / pitch bend: 2 bytes
        r.u8();
        r.u8();
      } else {
        throw std::runtime_error("unexpected MIDI status byte");
      }
    }
    // Close notes left hanging at the end of the track.
    for (auto& kv : open) s.notes[kv.second].tick_off = tick;
    s.end_tick = std::max(s.end_tick, tick);
    r.p = tend;
  }

  if (s.tempos.empty()) s.tempos.push_back({0, 500000});
  std::stable_sort(s.tempos.begin(), s.tempos.end(),
                   [](const MidiTempo& a, const MidiTempo& b) {
                     return a.tick < b.tick;
                   });
  if (s.tempos.front().tick != 0)
    s.tempos.insert(s.tempos.begin(), {0, s.tempos.front().usec_per_qn});

  std::stable_sort(s.notes.begin(), s.notes.end(),
                   [](const MidiNote& a, const MidiNote& b) {
                     return a.tick_on < b.tick_on;
                   });

  // Bar lines from the first time signature (4/4 if none); mid-song meter
  // changes are not yet split out.
  const int tpm = timesigs.empty() ? s.division * 4 : timesigs.front().second;
  for (long bt = 0; bt <= s.end_tick && tpm > 0; bt += tpm) s.bar_ticks.push_back(bt);

  return s;
}

}  // namespace circus2bmson
