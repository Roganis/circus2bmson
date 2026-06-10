#include "circus2bmson/convert.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "circus2bmson/bmson.hpp"
#include "circus2bmson/mod.hpp"
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

  const std::string doc = build_bmson_skeleton(mod, tl);

  namespace fs = std::filesystem;
  fs::path out_dir(opts.output_dir);
  if (!out_dir.empty()) fs::create_directories(out_dir);
  const std::string stem = fs::path(input_path).stem().string();
  const fs::path out_path = out_dir / (stem + ".bmson");

  std::ofstream of(out_path, std::ios::binary);
  if (!of) throw std::runtime_error("cannot write output: " + out_path.string());
  of << doc;
  of.close();

  ConvertResult r;
  r.bmson_path = out_path.string();
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
  return r;
}

}  // namespace circus2bmson
