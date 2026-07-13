#include "circus2bmson/furnace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <dr_libs/dr_wav.h>
#include <zlib.h>

#include "furnaceconv.hpp"
#include "stemconv.hpp"

namespace circus2bmson {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr char kPathSep = ';';
constexpr const char* kExeSuffix = ".exe";
#else
constexpr char kPathSep = ':';
constexpr const char* kExeSuffix = "";
#endif

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}

// Wrap a path for the shell std::system() hands the command to. The command is
// a string, so a path containing a quote could break out of it -- refuse rather
// than build something we cannot reason about.
std::string quote(const std::string& s) {
  if (s.find('"') != std::string::npos || s.find('\n') != std::string::npos)
    throw std::runtime_error(
        "path contains a quote or newline, which cannot be passed safely to "
        "furnace: " + s);
  return "\"" + s + "\"";
}

// std::system() hands the string to `cmd /c` on Windows, which strips the
// command's outermost quote pair unless the whole thing is one bare quoted
// program name -- ours is not, since it redirects and quotes several paths. The
// strip lands on the opening quote of the binary and the closing quote of the
// last path, leaving a mangled command line that cmd rejects with "the system
// cannot find the path specified". Quoting the command itself gives cmd a pair
// it can safely take away.
std::string shell_command(const std::string& cmd) {
#ifdef _WIN32
  return "\"" + cmd + "\"";
#else
  return cmd;
#endif
}

// Both formats are (usually) whole-file zlib streams. Returns the input
// unchanged when it is not one -- .fur is sometimes stored uncompressed.
std::vector<std::uint8_t> inflate_all(const std::vector<std::uint8_t>& in) {
  if (in.size() < 2 || in[0] != 0x78) return in;  // not a zlib stream

  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) return in;
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());

  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> chunk(64 * 1024);
  int rc = Z_OK;
  do {
    zs.next_out = chunk.data();
    zs.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) break;
    out.insert(out.end(), chunk.data(), chunk.data() + (chunk.size() - zs.avail_out));
  } while (rc != Z_STREAM_END);
  inflateEnd(&zs);
  return out.empty() ? in : out;
}

// What we need out of the module itself. Both formats state all of it in their
// header, which is far less work than parsing their pattern data:
//
//   hz  -- ticks per second: the rate Furnace runs the song at, and so the unit
//          its command stream timestamps notes in. Turns ticks into frames.
//   bpm -- the real musical tempo, from the row duration and the composer's own
//          "rows per beat" highlight. Beats guessing it from note spacing: the
//          notes are already exactly placed, and this makes the bmson's grid
//          line up with the bars the module was written on.
struct Timing {
  double hz = 0.0;   // 0 -> unknown; fall back to audio-domain onsets
  double bpm = 0.0;  // 0 -> unknown; fall back to inferring it from the onsets
};

// rows/beat is the editor highlight the composer set. Row duration is
// (timeBase + 1) * speed ticks; speed alternates between two values per row, so
// the average is what a tempo means here.
double bpm_from(double hz, int time_base, int speed1, int speed2, int rows_per_beat) {
  if (hz <= 0.0 || rows_per_beat <= 0 || speed1 <= 0 || speed2 <= 0) return 0.0;
  const double ticks_per_row = (time_base + 1) * (speed1 + speed2) / 2.0;
  if (ticks_per_row <= 0.0) return 0.0;
  const double bpm = 60.0 * hz / (ticks_per_row * rows_per_beat);
  return (bpm > 20.0 && bpm < 999.0) ? bpm : 0.0;
}

Timing module_timing(const std::vector<std::uint8_t>& raw, const std::string& fmt) {
  const std::vector<std::uint8_t> b = inflate_all(raw);
  Timing t;

  if (fmt == "fur") {
    // INFO block: "INFO", size, timeBase, speed1, speed2, arpTime, hz (float),
    // pattern length, orders length, highlight A, highlight B.
    for (std::size_t i = 0; i + 22 <= b.size(); ++i) {
      if (b[i] == 'I' && b[i + 1] == 'N' && b[i + 2] == 'F' && b[i + 3] == 'O') {
        float hz = 0.0f;
        std::memcpy(&hz, &b[i + 12], 4);
        if (hz <= 1.0f || hz >= 1000.0f) return t;
        t.hz = hz;
        t.bpm = bpm_from(t.hz, b[i + 8], b[i + 9], b[i + 10], b[i + 20]);
        return t;
      }
    }
    return t;
  }

  // .dmf header.
  const char* magic = ".DelekDefleMask.";
  if (b.size() < 32 || std::memcmp(b.data(), magic, 16) != 0) return t;
  std::size_t p = 16;
  p += 1;  // format version
  p += 1;  // system
  if (p >= b.size()) return t;
  p += 1 + b[p];  // song name (length-prefixed)
  if (p >= b.size()) return t;
  p += 1 + b[p];  // song author
  if (p + 8 > b.size()) return t;
  const int highlight_a = b[p];  // rows per beat
  p += 2;                        // highlight A / B
  const int time_base = b[p];
  const int speed1 = b[p + 1];
  const int speed2 = b[p + 2];
  p += 3;
  if (p + 5 > b.size()) return t;
  const int frames_mode = b[p];  // 0 = PAL, 1 = NTSC
  const int custom_on = b[p + 1];
  const char hz_txt[4] = {static_cast<char>(b[p + 2]), static_cast<char>(b[p + 3]),
                          static_cast<char>(b[p + 4]), '\0'};  // 3 ASCII digits

  t.hz = frames_mode == 1 ? 60.0 : 50.0;
  if (custom_on) {
    const int hz = std::atoi(hz_txt);
    if (hz > 1 && hz < 1000) t.hz = hz;
  }
  t.bpm = bpm_from(t.hz, time_base, speed1, speed2, highlight_a);
  return t;
}

// Note-ons from Furnace's `-view commands` log: lines of the shape
//
//     72 | 0: NOTE_ON(60, 15)
//     ^tick ^channel
//
// This is the whole point of using it -- a tracker issues NOTE_ON for *every*
// pattern note, including one that only changes pitch while the envelope keeps
// sounding, which is exactly what audio-domain onset detection cannot see.
//
// With -outmode perchan the song is replayed once per channel and the log
// repeats verbatim (ticks restarting at 0 each pass), so dedupe on (tick,
// channel) -- one channel cannot start two notes on the same tick.
struct NoteEvent {
  long frame = 0;
  int id = 0;  // identity of the sound: (instrument, note, volume)
};

std::map<int, std::vector<NoteEvent>> parse_note_ons(const fs::path& cmds,
                                                     double frames_per_tick,
                                                     long total_frames,
                                                     int sample_rate) {
  std::ifstream in(cmds);
  // (tick, channel) -> the note it starts. Dedupes the repeated passes.
  std::map<std::pair<long, int>, std::array<int, 3>> seen;  // -> {ins, note, vol}
  std::map<int, int> instrument;  // channel -> instrument currently selected
  std::string line;
  while (std::getline(in, line)) {
    long tick = 0;
    int chan = 0;
    char name[32] = {0};
    int a = 0, b = 0;
    const int got = std::sscanf(line.c_str(), " %ld | %d: %31[A-Z_](%d, %d)",
                                &tick, &chan, name, &a, &b);
    if (got < 3 || tick < 0 || chan < 0) continue;

    // The instrument is selected by its own command, before the note that uses
    // it. The same pitch on a different instrument is a different sound, so it
    // has to be part of the note's identity.
    if (std::strcmp(name, "INSTRUMENT") == 0 && got >= 4) {
      instrument[chan] = a;
      continue;
    }
    if (std::strcmp(name, "NOTE_ON") != 0 || got < 5) continue;
    seen[{tick, chan}] = {instrument.count(chan) ? instrument[chan] : -1, a, b};
  }

  // A looping song emits the loop point's note-ons at the tick *after* its last
  // one -- i.e. at the end of the audio, where there is nothing left to slice.
  // Furnace's render can overrun that tick by a frame or two, so requiring the
  // note to land strictly inside the buffer is not enough: demand that it has
  // some audible length, or it becomes a 2-frame keysound.
  const long min_slice = std::max<long>(1, sample_rate / 100);  // 10 ms

  std::map<std::array<int, 3>, int> ids;  // (ins, note, vol) -> small dense id
  std::map<int, std::vector<NoteEvent>> per_channel;
  for (const auto& kv : seen) {
    const long frame =
        static_cast<long>(std::llround(kv.first.first * frames_per_tick));
    if (frame + min_slice > total_frames) continue;
    auto it = ids.find(kv.second);
    if (it == ids.end())
      it = ids.emplace(kv.second, static_cast<int>(ids.size())).first;
    per_channel[kv.first.second].push_back({frame, it->second});
  }
  for (auto& kv : per_channel)
    std::sort(kv.second.begin(), kv.second.end(),
              [](const NoteEvent& x, const NoteEvent& y) {
                return x.frame < y.frame;
              });
  return per_channel;
}

// std::system returns a wait status, not an exit code: a plain failure comes
// back as 256, which is a confusing thing to show someone.
int exit_code(int status) {
#ifndef _WIN32
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return -WTERMSIG(status);
#endif
  return status;
}

// Furnace logs its real complaint ("could not open file!") and then keeps
// chattering about config files, so the tail of the log is the least useful
// part of it. Pull out the lines it marked as errors; fall back to the tail.
std::string furnace_error(const fs::path& log) {
  std::ifstream in(log);
  if (!in) return "";

  std::string line, errors, all;
  while (std::getline(in, line)) {
    all += line + "\n";
    // Strip the ANSI colour codes Furnace emits even when redirected.
    std::string plain;
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '\x1b') {
        while (i < line.size() && line[i] != 'm') ++i;
      } else {
        plain += line[i];
      }
    }
    if (plain.find("[ERROR]") != std::string::npos ||
        plain.find("[error]") != std::string::npos)
      errors += "  " + plain + "\n";
  }
  if (!errors.empty()) return ":\n" + errors;
  if (all.size() > 500) all = all.substr(all.size() - 500);
  return all.empty() ? "" : ":\n" + all;
}

// A temp directory that removes itself, so a failed export doesn't leave stems
// behind.
struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& tag) {
    static int counter = 0;
    std::ostringstream name;
    name << "c2b-furnace-" << tag << "-" << ++counter;
    path = fs::temp_directory_path() / name.str();
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);  // best effort
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

// Load an exported stem as interleaved stereo float. Furnace writes 16-bit
// stereo by default, but read whatever it gives us and widen mono if needed.
std::vector<float> read_wav_stereo(const fs::path& path, int& rate) {
  unsigned channels = 0;
  unsigned sample_rate = 0;
  drwav_uint64 frames = 0;
  float* data = drwav_open_file_and_read_pcm_frames_f32(
      path.string().c_str(), &channels, &sample_rate, &frames, nullptr);
  if (!data) throw std::runtime_error("cannot read stem: " + path.string());

  rate = static_cast<int>(sample_rate);
  std::vector<float> out(static_cast<std::size_t>(frames) * 2);
  for (drwav_uint64 f = 0; f < frames; ++f) {
    const float l = data[f * channels];
    const float r = channels > 1 ? data[f * channels + 1] : l;
    out[static_cast<std::size_t>(f) * 2] = l;
    out[static_cast<std::size_t>(f) * 2 + 1] = r;
  }
  drwav_free(data, nullptr);
  return out;
}

// Run Furnace's headless per-channel export and load the stems back.
//
// `furnace -output out.wav -outmode perchan` strips the trailing ".wav" and
// writes out_c01.wav, out_c02.wav, ... (1-indexed, zero-padded to 2). The
// export plays the song through once by default -- loops defaults to 0 and
// there is no fade-out -- which is exactly what slicing wants.
StemSong render_stems(const std::string& bin, const std::string& input_path,
                      const std::vector<std::uint8_t>& raw,
                      const ConvertOptions& opts) {
  const fs::path input = fs::absolute(input_path);
  TempDir tmp(fs::path(input_path).stem().string());
  const fs::path out_base = tmp.path / "out";
  const fs::path log = tmp.path / "furnace.log";

  // max_loops counts total plays; Furnace's -loops counts *extra* ones.
  const int extra = opts.max_loops > 1 ? opts.max_loops - 1 : 0;

  // -view commands dumps every engine command with its tick, on stdout; that is
  // where the real note events come from. stderr stays separate so the log we
  // quote back on failure is not full of note spam.
  const fs::path cmds = tmp.path / "commands.txt";
  std::ostringstream cmd;
  cmd << quote(bin) << " -loglevel error -noreport -nostatus -view commands"
      << " -loops " << extra << " -outmode perchan"
      << " -output " << quote((tmp.path / "out.wav").string()) << " "
      << quote(input.string()) << " > " << quote(cmds.string()) << " 2> "
      << quote(log.string());

  report(opts, "rendering chip channels with Furnace (a long song takes a "
                "while -- each channel is a separate playback pass)...");

  // Furnace renders one channel per pass and writes each file as it finishes,
  // so watching the directory fill up is the only progress signal available
  // from a subprocess. The count is all we can report -- the channel total is
  // not known until it is done.
  std::atomic<bool> done{false};
  std::thread watcher;
  if (opts.on_progress) {
    watcher = std::thread([&] {
      int seen = 0;
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(tmp.path, ec))
          if (e.path().extension() == ".wav") ++n;
        if (n > seen && !done.load()) {
          seen = n;
          report(opts, "  ...rendered channel " + std::to_string(seen));
        }
      }
    });
  }

  const int rc = std::system(shell_command(cmd.str()).c_str());
  done.store(true);
  if (watcher.joinable()) watcher.join();

  if (rc != 0)
    throw std::runtime_error("furnace export failed (exit " +
                             std::to_string(exit_code(rc)) + ")" +
                             furnace_error(log));

  // Collect out_c01.wav, out_c02.wav, ... until one is missing. Furnace groups
  // operator-split channels (e.g. Genesis extended CH3) into a single file, so
  // there can be fewer stems than the module has channels -- which is why we
  // count files rather than assume a channel count.
  StemSong song;
  int rate = 0;
  for (int i = 1;; ++i) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_c%02d.wav", i);
    const fs::path stem_path = out_base.string() + suffix;
    if (!fs::exists(stem_path)) break;

    int this_rate = 0;
    std::vector<float> pcm = read_wav_stereo(stem_path, this_rate);
    if (rate == 0) rate = this_rate;
    song.stems.push_back(std::move(pcm));
    song.voice_names.push_back("channel" + std::to_string(i));
  }

  if (song.stems.empty())
    throw std::runtime_error(
        "furnace produced no channel stems for " + input_path +
        " (unsupported or corrupt module?)");

  // The stems are independent playback passes, so guard against ragged lengths
  // rather than trusting them to match: pad every stem to the longest.
  std::size_t longest = 0;
  for (const std::vector<float>& s : song.stems)
    longest = std::max(longest, s.size());
  for (std::vector<float>& s : song.stems) s.resize(longest, 0.0f);

  song.sample_rate = rate > 0 ? rate : 44100;
  song.total_frames = static_cast<long>(longest / 2);
  song.format = lower(fs::path(input_path).extension().string());
  if (!song.format.empty() && song.format[0] == '.') song.format.erase(0, 1);

  char msg[128];
  std::snprintf(msg, sizeof(msg), "Furnace rendered %zu channels (%.1f s)",
                song.stems.size(),
                static_cast<double>(song.total_frames) / song.sample_rate);
  report(opts, msg);

  // The module's real note events, if we can line them up with the stems.
  const Timing timing = module_timing(raw, song.format);
  song.bpm = timing.bpm;  // 0 -> convert_stems infers one
  const double hz = timing.hz;
  if (hz > 0.0) {
    const double fpt = song.sample_rate / hz;
    const std::map<int, std::vector<NoteEvent>> notes =
        parse_note_ons(cmds, fpt, song.total_frames, song.sample_rate);

    // Furnace merges operator-split channels (Genesis extended CH3) into one
    // stem, so a command-stream channel index does not always address the stem
    // it sounds on. Only trust the mapping when the counts agree; otherwise the
    // notes would land on the wrong keysounds, and detected onsets -- lossy as
    // they are -- are the safer answer.
    const int highest = notes.empty() ? -1 : notes.rbegin()->first;
    if (!notes.empty() && highest < static_cast<int>(song.stems.size())) {
      song.onsets.assign(song.stems.size(), {});
      song.onset_ids.assign(song.stems.size(), {});
      long total = 0;
      for (const auto& kv : notes) {
        for (const NoteEvent& e : kv.second) {
          song.onsets[kv.first].push_back(e.frame);
          song.onset_ids[kv.first].push_back(e.id);
        }
        total += static_cast<long>(kv.second.size());
      }
      char m[160];
      if (timing.bpm > 0.0)
        std::snprintf(m, sizeof(m),
                      "read %ld note events from Furnace's command stream "
                      "(%g Hz tick rate, %g BPM)",
                      total, hz, timing.bpm);
      else
        std::snprintf(m, sizeof(m),
                      "read %ld note events from Furnace's command stream "
                      "(%g Hz tick rate)",
                      total, hz);
      report(opts, m);
    } else if (!notes.empty()) {
      report(opts,
             "note events span " + std::to_string(highest + 1) +
                 " channels but Furnace merged them into " +
                 std::to_string(song.stems.size()) +
                 " stems; falling back to onset detection");
    }
  }
  return song;
}

}  // namespace

std::vector<std::string> furnace_extensions() { return {"dmf", "fur"}; }

bool furnace_handles_extension(const std::string& ext) {
  std::string e = ext;
  if (!e.empty() && e[0] == '.') e.erase(0, 1);
  e = lower(e);
  for (const std::string& x : furnace_extensions())
    if (x == e) return true;
  return false;
}

std::string resolve_furnace(const std::string& given) {
  if (!given.empty()) {
    if (!fs::exists(given))
      throw std::runtime_error("furnace binary not found: " + given);
    return given;
  }
  if (const char* env = std::getenv("C2B_FURNACE")) {
    if (*env) {
      if (!fs::exists(env))
        throw std::runtime_error(
            "furnace binary not found (from $C2B_FURNACE): " +
            std::string(env));
      return env;
    }
  }
  if (const char* path = std::getenv("PATH")) {
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, kPathSep)) {
      if (dir.empty()) continue;
      const fs::path candidate =
          fs::path(dir) / (std::string("furnace") + kExeSuffix);
      std::error_code ec;
      if (fs::exists(candidate, ec)) return candidate.string();
    }
  }
  throw std::runtime_error(
      "Furnace not found -- .dmf/.fur conversion renders the module's channels "
      "with the Furnace tracker.\n"
      "Install it (Arch: pacman -S furnace; Debian: apt install furnace; "
      "or https://tildearrow.org/furnace/), then either put it on PATH, set "
      "$C2B_FURNACE, or pass --furnace <path>.");
}

ConvertResult convert_furnace(const std::vector<std::uint8_t>& bytes,
                              const std::string& input_path,
                              const ConvertOptions& opts) {
  // Furnace renders from the file itself; we read `bytes` only for the tick
  // rate, which turns its command stream's ticks into frames.
  const std::string bin = resolve_furnace(opts.furnace_path);
  const StemSong song = render_stems(bin, input_path, bytes, opts);
  return convert_stems(song, input_path, opts);
}

}  // namespace circus2bmson
