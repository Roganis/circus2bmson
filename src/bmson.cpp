#include "circus2bmson/bmson.hpp"

#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace circus2bmson {

using json = nlohmann::ordered_json;

std::string build_bmson_skeleton(const Module& mod, const Timeline& tl) {
  json doc;
  doc["version"] = "1.0.0";

  json info;
  info["title"] = mod.title;
  info["subtitle"] = "";
  info["artist"] = "";
  info["subartists"] = json::array();
  info["genre"] = "";
  info["mode_hint"] = "beat-7k";
  info["chart_name"] = "";
  info["judge_rank"] = 100;
  info["total"] = 100;
  info["init_bpm"] = tl.init_bpm;
  info["resolution"] = tl.resolution;
  doc["info"] = info;

  json lines = json::array();
  for (long y : tl.lines) {
    json l;
    l["y"] = y;
    lines.push_back(std::move(l));
  }
  doc["lines"] = std::move(lines);

  json bpm = json::array();
  for (const BpmEvent& e : tl.bpm_events) {
    json b;
    b["y"] = e.pulse;
    b["bpm"] = e.bpm;
    bpm.push_back(std::move(b));
  }
  doc["bpm_events"] = std::move(bpm);
  doc["stop_events"] = json::array();

  // Provisional keysounds: one channel per (sample, period). std::map keeps the
  // output deterministic (sorted by key); note lists stay in pulse order.
  std::map<std::pair<int, int>, std::vector<long>> groups;
  for (const NoteEvent& n : tl.notes)
    groups[{n.sample, n.period}].push_back(n.pulse);

  json channels = json::array();
  for (const auto& kv : groups) {
    char name[32];
    std::snprintf(name, sizeof(name), "s%02d_p%04d.wav", kv.first.first,
                  kv.first.second);
    json ch;
    ch["name"] = name;
    json notes = json::array();
    for (long y : kv.second) {
      json note;
      note["x"] = 0;  // BGM lane
      note["y"] = y;
      note["l"] = 0;
      note["c"] = false;
      notes.push_back(std::move(note));
    }
    ch["notes"] = std::move(notes);
    channels.push_back(std::move(ch));
  }
  doc["sound_channels"] = std::move(channels);

  return doc.dump(2);
}

}  // namespace circus2bmson
