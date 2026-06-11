#include "circus2bmson/convert.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "circus2bmson/bmson.hpp"
#include "circus2bmson/mod.hpp"
#include "circus2bmson/render.hpp"
#include "circus2bmson/timeline.hpp"

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
  const Module mod = parse_mod(bytes);

  TimelineOptions topts;
  topts.max_loops = opts.max_loops;
  const Timeline tl = build_timeline(mod, topts);

  namespace fs = std::filesystem;
  fs::path out_dir(opts.output_dir);
  if (!out_dir.empty()) fs::create_directories(out_dir);
  const std::string stem = fs::path(input_path).stem().string();
  const fs::path out_path = out_dir / (stem + ".bmson");

  ConvertResult r;
  r.title = mod.title;
  r.channels = mod.channels;
  r.note_count = tl.notes.size();
  r.bpm_event_count = tl.bpm_events.size();
  r.line_count = tl.lines.size();
  r.init_bpm = tl.init_bpm;
  r.total_seconds = tl.total_seconds;
  r.total_pulses = tl.total_pulses;
  r.emitted_rows = tl.emitted_rows;
  r.loops_played = tl.loops_played;
  r.truncated = tl.truncated;
  r.unsupported_flow = tl.unsupported_flow;

  std::string doc;
  if (opts.render_audio) {
    const std::string dir = out_dir.empty() ? "." : out_dir.string();
    const RenderResult rr = render_keysounds(bytes, mod, dir,
                                             opts.keysound_naming,
                                             opts.volume_ramping);

    // Bind every timeline note to its rendered keysound via (order,row,channel).
    std::vector<SoundChannel> channels(rr.keysound_names.size());
    for (std::size_t i = 0; i < channels.size(); ++i)
      channels[i].name = rr.keysound_names[i];
    long missing = 0;
    for (const NoteEvent& n : tl.notes) {
      const auto it = rr.note_to_keysound.find(note_key(n.order, n.row, n.channel));
      if (it == rr.note_to_keysound.end()) {
        ++missing;
        continue;
      }
      channels[it->second].note_pulses.push_back(n.pulse);
    }
    std::vector<SoundChannel> used;
    for (SoundChannel& c : channels)
      if (!c.note_pulses.empty()) used.push_back(std::move(c));
    // Sort by filename so keysounds stay grouped in an editor's sound list.
    std::sort(used.begin(), used.end(),
              [](const SoundChannel& a, const SoundChannel& b) {
                return a.name < b.name;
              });

    doc = build_bmson(mod, tl, used);
    r.audio_rendered = true;
    r.total_slices = rr.total_slices;
    r.keysound_count = rr.unique_keysounds;
    r.missing_keysounds = missing;
  } else {
    doc = build_bmson_skeleton(mod, tl);
  }

  std::ofstream of(out_path, std::ios::binary);
  if (!of) throw std::runtime_error("cannot write output: " + out_path.string());
  of << doc;
  of.close();

  r.bmson_path = out_path.string();
  return r;
}

}  // namespace circus2bmson
