#include "circus2bmson/bmson.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace circus2bmson {

using json = nlohmann::ordered_json;

std::string build_bmson(const Score& score,
                        const std::vector<SoundChannel>& channels) {
  json doc;
  doc["version"] = "1.0.0";

  json info;
  info["title"] = score.title;
  info["subtitle"] = "";
  info["artist"] = "";
  info["subartists"] = json::array();
  info["genre"] = "";
  info["mode_hint"] = "beat-7k";
  info["chart_name"] = "";
  info["judge_rank"] = 100;
  info["total"] = 100;
  info["init_bpm"] = score.init_bpm;
  info["resolution"] = score.resolution;
  doc["info"] = info;

  json lines = json::array();
  for (long y : score.lines) {
    json l;
    l["y"] = y;
    lines.push_back(std::move(l));
  }
  doc["lines"] = std::move(lines);

  json bpm = json::array();
  for (const BpmEvent& e : score.bpm_events) {
    json b;
    b["y"] = e.pulse;
    b["bpm"] = e.bpm;
    bpm.push_back(std::move(b));
  }
  doc["bpm_events"] = std::move(bpm);
  doc["stop_events"] = json::array();

  json sound = json::array();
  for (const SoundChannel& c : channels) {
    std::vector<long> pulses = c.note_pulses;
    std::sort(pulses.begin(), pulses.end());
    json ch;
    ch["name"] = c.name;
    json notes = json::array();
    for (long y : pulses) {
      json note;
      note["x"] = 0;  // BGM lane
      note["y"] = y;
      note["l"] = 0;
      note["c"] = false;
      notes.push_back(std::move(note));
    }
    ch["notes"] = std::move(notes);
    sound.push_back(std::move(ch));
  }
  doc["sound_channels"] = std::move(sound);

  // Replace (don't throw on) invalid UTF-8 -- module/MIDI text can be in legacy
  // encodings like Shift-JIS or Latin-1.
  return doc.dump(2, ' ', false, json::error_handler_t::replace);
}

std::string build_bmson_skeleton(const Score& score, const std::string& ext) {
  // Provisional keysounds: one channel per (instrument, note), names sorted.
  std::map<std::pair<int, int>, std::vector<long>> groups;
  for (const ScoreNote& n : score.notes)
    groups[{n.instrument, n.note}].push_back(n.pulse);

  std::vector<SoundChannel> channels;
  channels.reserve(groups.size());
  for (const auto& kv : groups) {
    char name[32];
    std::snprintf(name, sizeof(name), "i%02d_n%03d%s", kv.first.first,
                  kv.first.second, ext.c_str());
    channels.push_back({name, kv.second});
  }
  return build_bmson(score, channels);
}

}  // namespace circus2bmson
