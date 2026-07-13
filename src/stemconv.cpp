#include "stemconv.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <dr_libs/dr_wav.h>

#include "circus2bmson/bmson.hpp"
#include "circus2bmson/onset.hpp"
#include "circus2bmson/score.hpp"
#include "oggenc.hpp"

namespace circus2bmson {
namespace {

constexpr float kSilence = 1.0e-4f;  // trailing-silence trim threshold

// Filename-safe token from a voice name: "Square 1" -> "square_1".
std::string sanitize(const std::string& s) {
  std::string out;
  bool underscore = false;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      underscore = false;
    } else if (!out.empty() && !underscore) {
      out.push_back('_');
      underscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

std::int16_t to_i16(float f) {
  float v = f * 32767.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  return static_cast<std::int16_t>(std::lrintf(v));
}

// Trim trailing silence and quantise stem[start, end) to interleaved int16.
std::vector<std::int16_t> slice_pcm(const std::vector<float>& stem, long start,
                                    long end) {
  const long n = static_cast<long>(stem.size() / 2);
  if (start < 0) start = 0;
  long e = std::min(end, n);
  while (e > start && std::fabs(stem[2 * (e - 1)]) < kSilence &&
         std::fabs(stem[2 * (e - 1) + 1]) < kSilence)
    --e;
  if (e <= start) e = std::min(start + 1, n);
  std::vector<std::int16_t> pcm;
  pcm.reserve(static_cast<std::size_t>(2 * (e - start)));
  for (long f = start; f < e; ++f) {
    pcm.push_back(to_i16(stem[2 * f]));
    pcm.push_back(to_i16(stem[2 * f + 1]));
  }
  return pcm;
}

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm,
               int rate) {
  drwav_data_format fmt{};
  fmt.container = drwav_container_riff;
  fmt.format = DR_WAVE_FORMAT_PCM;
  fmt.channels = 2;
  fmt.sampleRate = static_cast<drwav_uint32>(rate);
  fmt.bitsPerSample = 16;
  drwav wav;
  if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr))
    throw std::runtime_error("cannot write WAV: " + path);
  drwav_write_pcm_frames(&wav, pcm.size() / 2, pcm.data());
  drwav_uninit(&wav);
}

// Plausible constant BPM from merged, sorted onset times (seconds). Timing is
// exact at any BPM -- this only sets how pulses map to gridlines -- so a crude
// heuristic is safe: take the median onset gap as a 16th note, fold to a sane
// range. Falls back to 150 when there is too little to go on.
double infer_bpm(const std::vector<double>& onsets) {
  std::vector<double> gaps;
  for (std::size_t i = 1; i < onsets.size(); ++i) {
    const double g = onsets[i] - onsets[i - 1];
    if (g > 0.01 && g < 2.0) gaps.push_back(g);
  }
  if (gaps.empty()) return 150.0;
  std::sort(gaps.begin(), gaps.end());
  const double sixteenth = gaps[gaps.size() / 2];
  double bpm = 60.0 / (4.0 * sixteenth);
  while (bpm < 70.0) bpm *= 2.0;
  while (bpm > 280.0) bpm /= 2.0;
  return bpm;
}

// Encoding a keysound is pure CPU and each one is independent, so it is the one
// part of this worth threading -- a long chip song writes thousands of files,
// and OGG in particular dominates the wall clock.
//
// Slicing, dedup and naming stay on the calling thread: they are cheap, and
// keeping them serial keeps keysound ids and filenames deterministic (the same
// module must convert to the same bmson every time). Only the encode-and-write
// is handed off. The queue is bounded because the PCM waiting in it is the
// whole song's audio -- hundreds of MB for a long module if left unbounded.
class EncoderPool {
 public:
  EncoderPool(int rate, AudioFormat format, std::size_t workers)
      : rate_(rate), format_(format) {
    for (std::size_t i = 0; i < workers; ++i)
      threads_.emplace_back([this] { run(); });
  }

  void submit(std::string path, std::vector<std::int16_t> pcm) {
    std::unique_lock<std::mutex> lock(mtx_);
    space_.wait(lock, [this] { return queue_.size() < kMaxPending; });
    queue_.push({std::move(path), std::move(pcm)});
    work_.notify_one();
  }

  // Drain, stop the workers, and rethrow whatever the first one that failed hit
  // (a full disk must not pass silently for want of a return value).
  void finish() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      done_ = true;
    }
    work_.notify_all();
    for (std::thread& t : threads_) t.join();
    threads_.clear();
    if (error_) std::rethrow_exception(error_);
  }

  ~EncoderPool() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      done_ = true;
    }
    work_.notify_all();
    for (std::thread& t : threads_)
      if (t.joinable()) t.join();
  }

 private:
  struct Job {
    std::string path;
    std::vector<std::int16_t> pcm;
  };
  static constexpr std::size_t kMaxPending = 64;

  void run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mtx_);
        work_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return;  // done_ and drained
        job = std::move(queue_.front());
        queue_.pop();
      }
      space_.notify_one();
      try {
        if (format_ == AudioFormat::Ogg)
          write_ogg(job.path, job.pcm, rate_);
        else
          write_wav(job.path, job.pcm, rate_);
      } catch (...) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!error_) error_ = std::current_exception();
      }
    }
  }

  const int rate_;
  const AudioFormat format_;
  std::vector<std::thread> threads_;
  std::queue<Job> queue_;
  std::mutex mtx_;
  std::condition_variable work_, space_;
  bool done_ = false;
  std::exception_ptr error_;
};

}  // namespace

ConvertResult convert_stems(const StemSong& song, const std::string& input_path,
                            const ConvertOptions& opts) {
  namespace fs = std::filesystem;
  const int rate = song.sample_rate;
  const std::vector<std::vector<float>>& stems = song.stems;

  // Note-ons per voice. Real note events when the backend has them; otherwise
  // recover what we can from the audio (which misses any note that is only a
  // pitch change -- see onset.hpp).
  std::vector<std::vector<long>> onsets = song.onsets;
  if (onsets.size() == stems.size()) {
    report(opts, "using the module's own note events");
  } else {
    report(opts, "detecting note onsets in " + std::to_string(stems.size()) +
                     " channels...");
    onsets.assign(stems.size(), {});
    for (std::size_t v = 0; v < stems.size(); ++v)
      onsets[v] = detect_onsets(stems[v].data(),
                                static_cast<long>(stems[v].size() / 2), rate);
  }

  std::vector<double> merged;
  for (const std::vector<long>& v : onsets)
    for (long f : v) merged.push_back(static_cast<double>(f) / rate);
  std::sort(merged.begin(), merged.end());
  report(opts, "found " + std::to_string(merged.size()) + " notes");

  constexpr int kResolution = 480;
  const double bpm = song.bpm > 0.0 ? song.bpm : infer_bpm(merged);
  auto pulse_at = [&](long frame) {
    return static_cast<long>(std::llround(static_cast<double>(frame) / rate *
                                          bpm / 60.0 * kResolution));
  };

  fs::path out_dir(opts.output_dir);
  if (!out_dir.empty()) fs::create_directories(out_dir);
  const char* ext = audio_extension(opts.audio_format);

  // Slice each voice at its onsets, dedup identical PCM, place at exact time.
  std::unordered_map<std::string, int> dedup;  // raw PCM -> keysound id
  std::set<std::string> used_names;
  std::vector<SoundChannel> channels;
  long total_slices = 0, note_count = 0;
  const bool render = opts.render_audio;

  const long expected = static_cast<long>(merged.size());
  unsigned workers = std::thread::hardware_concurrency();
  if (workers == 0) workers = 2;
  workers = std::min(workers, 8u);  // I/O-bound past this; don't thrash
  if (render)
    report(opts, "slicing and encoding " + std::to_string(expected) +
                     " keysounds (" +
                     (opts.audio_format == AudioFormat::Ogg ? "ogg" : "wav") +
                     ", " + std::to_string(workers) + " threads)...");
  EncoderPool pool(rate, opts.audio_format, workers);
  long next_tick = 0;  // report roughly every 10%

  for (std::size_t v = 0; v < stems.size(); ++v) {
    const long n = static_cast<long>(stems[v].size() / 2);
    const std::string& voice = song.voice_names[v];
    int seq = 0;
    for (std::size_t k = 0; k < onsets[v].size(); ++k) {
      const long start = onsets[v][k];
      const long end = (k + 1 < onsets[v].size()) ? onsets[v][k + 1] : n;
      if (end <= start) continue;
      ++note_count;
      const long pulse = pulse_at(start);

      if (render && expected > 0 && note_count >= next_tick) {
        const int pct = static_cast<int>(100 * note_count / expected);
        report(opts, "  ...keysound " + std::to_string(note_count) + "/" +
                         std::to_string(expected) + "  (" +
                         std::to_string(pct) + "%)");
        next_tick = note_count + expected / 10 + 1;
      }

      int id;
      if (render) {
        std::vector<std::int16_t> pcm = slice_pcm(stems[v], start, end);
        ++total_slices;
        std::string raw(reinterpret_cast<const char*>(pcm.data()),
                        pcm.size() * sizeof(std::int16_t));
        auto found = dedup.find(raw);
        if (found == dedup.end()) {
          id = static_cast<int>(channels.size());
          std::string vn = sanitize(voice);
          if (vn.empty()) vn = "voice" + std::to_string(v + 1);
          char b[48];
          std::snprintf(b, sizeof(b), "%s_%03d", vn.c_str(), ++seq);
          std::string base(b);
          std::string name = base + ext;
          for (int x = 2; used_names.count(name); ++x)
            name = base + "_" + std::to_string(x) + ext;
          used_names.insert(name);
          pool.submit((out_dir / name).string(), std::move(pcm));
          SoundChannel sc;
          sc.name = name;
          channels.push_back(std::move(sc));
          dedup.emplace(std::move(raw), id);
        } else {
          id = found->second;
        }
      } else {  // skeleton: one channel per voice, no audio files
        id = static_cast<int>(v);
        if (static_cast<std::size_t>(id) >= channels.size())
          channels.resize(id + 1);
        if (channels[id].name.empty()) channels[id].name = voice + ext;
      }
      channels[id].note_pulses.push_back(pulse);
    }
  }

  pool.finish();  // waits for the encoders; rethrows if any of them failed

  if (render)
    report(opts, "wrote " + std::to_string(channels.size()) +
                     " unique keysounds from " + std::to_string(total_slices) +
                     " slices");

  std::vector<SoundChannel> used;
  for (SoundChannel& c : channels)
    if (!c.note_pulses.empty()) used.push_back(std::move(c));
  std::sort(used.begin(), used.end(),
            [](const SoundChannel& a, const SoundChannel& b) {
              return a.name < b.name;
            });

  // Minimal Score carrying just what build_bmson needs (header + grid).
  Score sc;
  sc.title = !song.title.empty() ? song.title
                                 : fs::path(input_path).stem().string();
  sc.format = song.format;
  sc.channels = static_cast<int>(stems.size());
  sc.sample_rate = rate;
  sc.resolution = kResolution;
  sc.init_bpm = bpm;
  sc.total_frames = song.total_frames;
  sc.total_seconds = static_cast<double>(song.total_frames) / rate;
  sc.total_pulses = pulse_at(song.total_frames);
  for (long p = 0; p <= sc.total_pulses; p += 4 * kResolution)
    sc.lines.push_back(p);  // a bar line every 4 beats

  const std::string stem = fs::path(input_path).stem().string();
  const fs::path out_path = out_dir / (stem + ".bmson");
  std::ofstream of(out_path, std::ios::binary);
  if (!of) throw std::runtime_error("cannot write output: " + out_path.string());
  of << (render ? build_bmson(sc, used) : build_bmson_skeleton(sc, ext));
  of.close();

  ConvertResult r;
  r.bmson_path = out_path.string();
  r.title = sc.title;
  r.format = sc.format;
  r.channels = sc.channels;
  r.note_count = static_cast<std::size_t>(note_count);
  r.line_count = sc.lines.size();
  r.init_bpm = bpm;
  r.total_seconds = sc.total_seconds;
  r.total_pulses = sc.total_pulses;
  r.audio_rendered = render;
  r.total_slices = total_slices;
  r.keysound_count = static_cast<long>(used.size());
  return r;
}

}  // namespace circus2bmson
