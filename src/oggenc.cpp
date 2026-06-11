#include "oggenc.hpp"

#include <cstdio>
#include <stdexcept>

#include <vorbis/vorbisenc.h>

namespace circus2bmson {
namespace {

struct FileCloser {
  std::FILE* f;
  ~FileCloser() {
    if (f) std::fclose(f);
  }
};

void write_page(std::FILE* f, const ogg_page& og) {
  if (std::fwrite(og.header, 1, og.header_len, f) !=
          static_cast<std::size_t>(og.header_len) ||
      std::fwrite(og.body, 1, og.body_len, f) !=
          static_cast<std::size_t>(og.body_len))
    throw std::runtime_error("short write while writing OGG page");
}

}  // namespace

void write_ogg(const std::string& path,
               const std::vector<std::int16_t>& pcm, int rate, float quality) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write OGG: " + path);
  FileCloser closer{f};

  vorbis_info vi;
  vorbis_info_init(&vi);
  if (vorbis_encode_init_vbr(&vi, 2, rate, quality) != 0) {
    vorbis_info_clear(&vi);
    throw std::runtime_error("vorbis_encode_init_vbr failed");
  }
  vorbis_comment vc;
  vorbis_comment_init(&vc);
  vorbis_comment_add_tag(&vc, "ENCODER", "circus2bmson");

  vorbis_dsp_state vd;
  vorbis_block vb;
  vorbis_analysis_init(&vd, &vi);
  vorbis_block_init(&vd, &vb);

  ogg_stream_state os;
  ogg_stream_init(&os, 1);

  ogg_packet hdr, hdr_comm, hdr_code;
  vorbis_analysis_headerout(&vd, &vc, &hdr, &hdr_comm, &hdr_code);
  ogg_stream_packetin(&os, &hdr);
  ogg_stream_packetin(&os, &hdr_comm);
  ogg_stream_packetin(&os, &hdr_code);
  ogg_page og;
  while (ogg_stream_flush(&os, &og) != 0) write_page(f, og);

  const std::size_t total_frames = pcm.size() / 2;
  std::size_t pos = 0;
  ogg_packet op;
  bool eos = false;
  while (!eos) {
    const std::size_t n = std::min<std::size_t>(1024, total_frames - pos);
    if (n == 0) {
      vorbis_analysis_wrote(&vd, 0);  // end of stream
    } else {
      float** ch = vorbis_analysis_buffer(&vd, static_cast<int>(n));
      for (std::size_t i = 0; i < n; ++i) {
        ch[0][i] = pcm[2 * (pos + i)] / 32768.0f;
        ch[1][i] = pcm[2 * (pos + i) + 1] / 32768.0f;
      }
      vorbis_analysis_wrote(&vd, static_cast<int>(n));
      pos += n;
    }
    while (vorbis_analysis_blockout(&vd, &vb) == 1) {
      vorbis_analysis(&vb, nullptr);
      vorbis_bitrate_addblock(&vb);
      while (vorbis_bitrate_flushpacket(&vd, &op)) {
        ogg_stream_packetin(&os, &op);
        while (!eos && ogg_stream_pageout(&os, &og) != 0) {
          write_page(f, og);
          if (ogg_page_eos(&og)) eos = true;
        }
      }
    }
    if (n == 0 && !eos) {
      while (ogg_stream_flush(&os, &og) != 0) {
        write_page(f, og);
        if (ogg_page_eos(&og)) eos = true;
      }
      eos = true;
    }
  }

  ogg_stream_clear(&os);
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
}

}  // namespace circus2bmson
