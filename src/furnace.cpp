#include "circus2bmson/furnace.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <dr_libs/dr_wav.h>

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
                      const ConvertOptions& opts) {
  const fs::path input = fs::absolute(input_path);
  TempDir tmp(fs::path(input_path).stem().string());
  const fs::path out_base = tmp.path / "out";
  const fs::path log = tmp.path / "furnace.log";

  // max_loops counts total plays; Furnace's -loops counts *extra* ones.
  const int extra = opts.max_loops > 1 ? opts.max_loops - 1 : 0;

  std::ostringstream cmd;
  cmd << quote(bin) << " -loglevel error -noreport -nostatus"
      << " -loops " << extra << " -outmode perchan"
      << " -output " << quote((tmp.path / "out.wav").string()) << " "
      << quote(input.string()) << " > " << quote(log.string()) << " 2>&1";

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

  const int rc = std::system(cmd.str().c_str());
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
  (void)bytes;  // Furnace reads the file itself
  const std::string bin = resolve_furnace(opts.furnace_path);
  const StemSong song = render_stems(bin, input_path, opts);
  return convert_stems(song, input_path, opts);
}

}  // namespace circus2bmson
