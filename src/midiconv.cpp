#include "midiconv.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <dr_libs/dr_wav.h>

#include "circus2bmson/bmson.hpp"
#include "circus2bmson/midi.hpp"
#include "circus2bmson/render.hpp"
#include "circus2bmson/score.hpp"
#include "midisynth.hpp"
#include "oggenc.hpp"

namespace circus2bmson {
namespace {

constexpr int kRate = 44100;
constexpr float kSilence = 1.0e-4f;

std::int16_t to_i16(float f) {
  float v = f * 32767.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  return static_cast<std::int16_t>(std::lrintf(v));
}

std::string pad(int v, int w) {
  char b[16];
  std::snprintf(b, sizeof(b), "%0*d", w, v);
  return b;
}

std::string note_name(int key) {
  static const char* n[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                              "F#", "G",  "G#", "A",  "A#", "B"};
  const std::string nm = n[key % 12];
  const int oct = key / 12 - 1;
  return nm.size() == 1 ? nm + "-" + std::to_string(oct) : nm + std::to_string(oct);
}

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm) {
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_PCM;
  fmt.channels = 2;
  fmt.sampleRate = kRate;
  fmt.bitsPerSample = 16;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write WAV: " + path);
  drwav_write_pcm_frames(&wav, pcm.size() / 2, pcm.data());
  drwav_uninit(&wav);
}

// Trim trailing silence and quantise interleaved float -> int16.
std::vector<std::int16_t> to_pcm(const std::vector<float>& f) {
  long e = static_cast<long>(f.size() / 2);
  while (e > 0) {
    if (std::fabs(f[2 * (e - 1)]) >= kSilence ||
        std::fabs(f[2 * (e - 1) + 1]) >= kSilence)
      break;
    --e;
  }
  if (e < 1) e = 1;
  std::vector<std::int16_t> pcm;
  pcm.reserve(static_cast<std::size_t>(2 * e));
  for (long i = 0; i < e; ++i) {
    pcm.push_back(to_i16(f[2 * i]));
    pcm.push_back(to_i16(f[2 * i + 1]));
  }
  return pcm;
}

std::string resolve_soundfont(const std::string& given) {
  namespace fs = std::filesystem;
  if (!given.empty()) {
    if (fs::exists(given)) return given;
    throw std::runtime_error("SoundFont not found: " + given);
  }
  if (const char* env = std::getenv("C2B_SOUNDFONT"))
    if (env[0] && fs::exists(env)) return env;
  const char* candidates[] = {
      "/usr/share/sounds/sf2/FluidR3_GM.sf2",
      "/usr/share/sounds/sf2/TimGM6mb.sf2",
      "/usr/share/sounds/sf2/default-GM.sf2",
      "/usr/share/soundfonts/FluidR3_GM.sf2",
      "/usr/share/soundfonts/default.sf2",
  };
  for (const char* c : candidates)
    if (fs::exists(c)) return c;
  throw std::runtime_error(
      "no SoundFont found; supply one with --soundfont or the GUI picker "
      "(MIDI files carry no audio of their own)");
}

// tick -> absolute seconds, from the tempo map.
class TempoMap {
 public:
  TempoMap(const std::vector<MidiTempo>& t, int division) : t_(t) {
    spt_.resize(t.size());
    cum_.resize(t.size());
    cum_[0] = 0.0;
    spt_[0] = t[0].usec_per_qn / 1.0e6 / division;
    for (std::size_t i = 1; i < t.size(); ++i) {
      spt_[i] = t[i].usec_per_qn / 1.0e6 / division;
      cum_[i] = cum_[i - 1] +
                static_cast<double>(t[i].tick - t[i - 1].tick) * spt_[i - 1];
    }
  }
  double seconds(long tick) const {
    std::size_t i = 0;
    while (i + 1 < t_.size() && t_[i + 1].tick <= tick) ++i;
    return cum_[i] + static_cast<double>(tick - t_[i].tick) * spt_[i];
  }

 private:
  const std::vector<MidiTempo>& t_;
  std::vector<double> spt_, cum_;
};

}  // namespace

ConvertResult convert_midi(const std::vector<std::uint8_t>& bytes,
                           const std::string& input_path,
                           const ConvertOptions& opts) {
  const MidiSong song = parse_smf(bytes);
  const TempoMap tempo(song.tempos, song.division);

  namespace fs = std::filesystem;
  fs::path out_dir(opts.output_dir);
  if (!out_dir.empty()) fs::create_directories(out_dir);
  const std::string stem = fs::path(input_path).stem().string();
  const fs::path out_path = out_dir / (stem + ".bmson");
  const std::string dir = out_dir.empty() ? "." : out_dir.string();
  const char* ext = audio_extension(opts.audio_format);

  // --- bmson timeline (1 pulse == 1 MIDI tick). ---
  Score sc;
  sc.title = !song.title.empty() ? song.title : stem;
  sc.format = "midi";
  sc.resolution = song.division;
  sc.init_bpm = 60000000.0 / song.tempos[0].usec_per_qn;
  for (std::size_t i = 1; i < song.tempos.size(); ++i)
    sc.bpm_events.push_back(
        {song.tempos[i].tick, 60000000.0 / song.tempos[i].usec_per_qn});
  for (long bt : song.bar_ticks) sc.lines.push_back(bt);
  sc.total_pulses = song.end_tick;
  sc.total_seconds = tempo.seconds(song.end_tick);

  // --- render / dedup keysounds, one per (program,key,vel,drum,frames). ---
  std::unique_ptr<MidiSynth> synth;
  if (opts.render_audio) {
    synth = std::make_unique<MidiSynth>(resolve_soundfont(opts.soundfont_path),
                                        kRate);
    if (!synth->ok())
      throw std::runtime_error("could not load SoundFont (FluidSynth)");
  }

  struct Key {
    int program, key, vel, drum;
    long frames;
    bool operator<(const Key& o) const {
      return std::tie(program, key, vel, drum, frames) <
             std::tie(o.program, o.key, o.vel, o.drum, o.frames);
    }
  };
  std::map<Key, int> dedup;            // note signature -> keysound id
  std::vector<SoundChannel> channels;  // id-indexed
  std::vector<int> chan_seq(16, 0);
  long total_slices = 0;

  for (const MidiNote& n : song.notes) {
    ++total_slices;
    const double dur = tempo.seconds(n.tick_off) - tempo.seconds(n.tick_on);
    const long frames = std::max<long>(1, std::lround(dur * kRate));
    const Key sig{n.program, n.key, n.velocity, n.drum ? 1 : 0, frames};

    auto it = dedup.find(sig);
    int id;
    if (it == dedup.end()) {
      id = static_cast<int>(channels.size());
      std::string base;
      if (opts.keysound_naming == KeysoundNaming::Channel) {
        base = "channel" + std::to_string(n.channel + 1) + "_" +
               pad(++chan_seq[n.channel], 3);
      } else {
        const std::string instr =
            n.drum ? "drum" + pad(n.key, 2)
                   : "prog" + pad(n.program, 3) + "_" + note_name(n.key);
        const std::string cht = "ch" + pad(n.channel + 1, 2);
        base = opts.keysound_naming == KeysoundNaming::Lane
                   ? cht + "_" + instr
                   : instr + "_" + cht;
      }
      const std::string name = base + ext;
      if (opts.render_audio) {
        const std::vector<std::int16_t> pcm = to_pcm(
            synth->render_note(n.program, n.key, n.velocity, n.drum, frames));
        if (opts.audio_format == AudioFormat::Ogg)
          write_ogg(dir + "/" + name, pcm, kRate);
        else
          write_wav(dir + "/" + name, pcm);
      }
      channels.push_back({name, {}});
      dedup.emplace(sig, id);
    } else {
      id = it->second;
    }
    channels[id].note_pulses.push_back(n.tick_on);
  }

  std::sort(channels.begin(), channels.end(),
            [](const SoundChannel& a, const SoundChannel& b) {
              return a.name < b.name;
            });
  const std::string doc = build_bmson(sc, channels);
  std::ofstream of(out_path, std::ios::binary);
  if (!of) throw std::runtime_error("cannot write output: " + out_path.string());
  of << doc;
  of.close();

  ConvertResult r;
  r.bmson_path = out_path.string();
  r.title = sc.title;
  r.format = sc.format;
  r.channels = 16;
  r.note_count = song.notes.size();
  r.bpm_event_count = sc.bpm_events.size();
  r.line_count = sc.lines.size();
  r.init_bpm = sc.init_bpm;
  r.total_seconds = sc.total_seconds;
  r.total_pulses = sc.total_pulses;
  r.audio_rendered = opts.render_audio;
  r.total_slices = total_slices;
  r.keysound_count = static_cast<long>(channels.size());
  return r;
}

}  // namespace circus2bmson
