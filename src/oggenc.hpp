#ifndef CIRCUS2BMSON_OGGENC_HPP
#define CIRCUS2BMSON_OGGENC_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace circus2bmson {

// Encode interleaved stereo 16-bit PCM to an Ogg Vorbis file (VBR).
// quality is libvorbis VBR quality, -0.1 .. 1.0. Throws on failure.
void write_ogg(const std::string& path,
               const std::vector<std::int16_t>& interleaved_stereo, int rate,
               float quality = 0.5f);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_OGGENC_HPP
