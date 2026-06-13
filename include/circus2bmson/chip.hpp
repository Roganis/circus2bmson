#ifndef CIRCUS2BMSON_CHIP_HPP
#define CIRCUS2BMSON_CHIP_HPP

#include <cstdint>
#include <string>
#include <vector>

// game-music-emu backend (spike). Load a chip-music file (NSF/GBS/VGM/SPC/...),
// render each emulated voice in isolation, and derive its note onsets from the
// audio (audio-domain detection -- chip formats carry no note events). This is
// the front half of the eventual chip -> bmson path; turning onsets into
// keysounds on a pulse grid comes next. Built only when libgme is available
// (C2B_HAVE_LIBGME); otherwise scan_chip throws and chip_supported() is false.
namespace circus2bmson {

struct ChipVoice {
  std::string name;                // emulator voice name, e.g. "Square 1"
  std::vector<long> onset_frames;  // detected note onsets (at sample_rate)
};

struct ChipScan {
  std::string type;                 // gme type extension, e.g. "nsf", "vgm"
  std::string system;               // e.g. "Nintendo NES"
  std::string game, song, author;   // track metadata ("" if absent)
  int sample_rate = 44100;
  long total_frames = 0;            // rendered length
  std::vector<ChipVoice> voices;
};

// True when this build was compiled with libgme support.
bool chip_supported();

// Load `bytes` (track 0), render per-voice stems, and detect onsets per voice.
// Throws std::runtime_error if libgme is unavailable or the data is unreadable.
ChipScan scan_chip(const std::vector<std::uint8_t>& bytes,
                   int sample_rate = 44100);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_CHIP_HPP
