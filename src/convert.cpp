#include "circus2bmson/convert.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include <libopenmpt/libopenmpt.hpp>

#include "circus2bmson/bmson.hpp"
#include "circus2bmson/chip.hpp"
#include "circus2bmson/furnace.hpp"
#include "circus2bmson/render.hpp"
#include "circus2bmson/score.hpp"
#include "chipconv.hpp"
#include "furnaceconv.hpp"
#include "midiconv.hpp"

namespace circus2bmson {
namespace {

std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open input file: " + path);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

}  // namespace

ConvertResult convert_mod_file(const std::string& input_path,
                               const ConvertOptions& opts) {
  const std::vector<std::uint8_t> bytes = read_file_bytes(input_path);

  // MIDI is a separate family (no embedded audio -> needs a SoundFont synth);
  // dispatch by extension since libopenmpt can't sniff it.
  std::string ext = std::filesystem::path(input_path).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".mid" || ext == ".midi")
    return convert_midi(bytes, input_path, opts);

  // Chiptune trackers (.dmf/.fur) need chip emulation to play: the Furnace
  // binary renders their channels for us (see furnace.hpp). Runtime dependency,
  // so this dispatches unconditionally and fails with an install hint.
  if (furnace_handles_extension(ext))
    return convert_furnace(bytes, input_path, opts);

  // Chip-music formats (NSF/GBS/VGM/...) need the libgme backend; libopenmpt
  // can't read them. Only dispatched here when libgme was built in.
  if (chip_handles_extension(ext))
    return convert_chip(bytes, input_path, opts);

  const Score score = read_score(bytes, opts.max_loops);

  namespace fs = std::filesystem;
  fs::path out_dir(opts.output_dir);
  if (!out_dir.empty()) fs::create_directories(out_dir);
  const std::string stem = fs::path(input_path).stem().string();
  const fs::path out_path = out_dir / (stem + ".bmson");

  ConvertResult r;
  r.title = score.title;
  r.format = score.format;
  r.channels = score.channels;
  r.note_count = score.notes.size();
  r.bpm_event_count = score.bpm_events.size();
  r.line_count = score.lines.size();
  r.init_bpm = score.init_bpm;
  r.total_seconds = score.total_seconds;
  r.total_pulses = score.total_pulses;
  r.emitted_rows = static_cast<long>(score.rows.size());
  r.truncated = score.truncated;
  r.coarse_rows = score.coarse_rows;

  std::string doc;
  if (opts.render_audio) {
    const std::string dir = out_dir.empty() ? "." : out_dir.string();
    const RenderResult rr =
        render_keysounds(bytes, score, dir, opts.keysound_naming,
                         opts.volume_ramping, opts.audio_format);

    std::vector<SoundChannel> channels(rr.keysound_names.size());
    for (std::size_t i = 0; i < channels.size(); ++i)
      channels[i].name = rr.keysound_names[i];
    for (std::size_t i = 0; i < score.notes.size(); ++i) {
      const int id = rr.note_keysound[i];
      if (id >= 0) channels[id].note_pulses.push_back(score.notes[i].pulse);
    }
    std::vector<SoundChannel> used;
    for (SoundChannel& c : channels)
      if (!c.note_pulses.empty()) used.push_back(std::move(c));
    // Sort by filename so keysounds stay grouped in an editor's sound list.
    std::sort(used.begin(), used.end(),
              [](const SoundChannel& a, const SoundChannel& b) {
                return a.name < b.name;
              });

    doc = build_bmson(score, used);
    r.audio_rendered = true;
    r.total_slices = rr.total_slices;
    r.keysound_count = rr.unique_keysounds;
  } else {
    doc = build_bmson_skeleton(score, audio_extension(opts.audio_format));
  }

  std::ofstream of(out_path, std::ios::binary);
  if (!of) throw std::runtime_error("cannot write output: " + out_path.string());
  of << doc;
  of.close();

  r.bmson_path = out_path.string();
  return r;
}

std::vector<std::string> supported_input_extensions() {
  std::vector<std::string> exts = openmpt::get_supported_extensions();
  exts.emplace_back("mid");
  exts.emplace_back("midi");
  for (const std::string& e : chip_extensions()) exts.push_back(e);  // {} w/o libgme
  // Furnace is a runtime dependency, so .dmf/.fur are always advertised; a
  // conversion without the binary installed fails with an install hint.
  for (const std::string& e : furnace_extensions()) exts.push_back(e);
  std::sort(exts.begin(), exts.end());
  exts.erase(std::unique(exts.begin(), exts.end()), exts.end());
  return exts;
}

}  // namespace circus2bmson
