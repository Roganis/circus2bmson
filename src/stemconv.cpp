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
#include <tuple>
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

// Where stem[start, end) really ends once trailing silence is dropped.
long trim_end(const std::vector<float>& stem, long start, long end) {
  const long n = static_cast<long>(stem.size() / 2);
  if (start < 0) start = 0;
  long e = std::min(end, n);
  while (e > start && std::fabs(stem[2 * (e - 1)]) < kSilence &&
         std::fabs(stem[2 * (e - 1) + 1]) < kSilence)
    --e;
  if (e <= start) e = std::min(start + 1, n);
  return e;
}

// Quantise stem[start, end) to interleaved int16. `end` is already trimmed.
std::vector<std::int16_t> slice_pcm(const std::vector<float>& stem, long start,
                                    long end) {
  std::vector<std::int16_t> pcm;
  pcm.reserve(static_cast<std::size_t>(2 * (end - start)));
  for (long f = start; f < end; ++f) {
    pcm.push_back(to_i16(stem[2 * f]));
    pcm.push_back(to_i16(stem[2 * f + 1]));
  }
  return pcm;
}

// Root-mean-square of stem[start, end), for the near-duplicate test below.
double slice_rms(const std::vector<float>& stem, long start, long end) {
  double sum = 0.0;
  for (long f = 2 * start; f < 2 * end; ++f)
    sum += static_cast<double>(stem[f]) * stem[f];
  const long n = 2 * (end - start);
  return n > 0 ? std::sqrt(sum / n) : 0.0;
}

// How far two equal-length slices are apart, as dB relative to the first: 0 dB
// is "completely different", -60 dB is "the same sound". Chip stems repeat a
// drum hit or a held note with a slightly different phase or envelope tail, so
// they are never bit-identical even when they are the same sound; this is what
// lets those collapse into one keysound.
//
// Both slices are read straight out of the stems, which are already in memory --
// keeping a copy of every unique keysound's PCM around just to compare against
// would cost hundreds of MB on a long module.
double residual_db(const std::vector<float>& a, long a0, const std::vector<float>& b,
                   long b0, long frames, double rms_a) {
  if (frames <= 0 || rms_a <= 0.0) return 0.0;
  double sum = 0.0;
  for (long f = 0; f < 2 * frames; ++f) {
    const double d = static_cast<double>(a[2 * a0 + f]) - b[2 * b0 + f];
    sum += d * d;
  }
  const double rms_d = std::sqrt(sum / (2 * frames));
  return 20.0 * std::log10(rms_d / rms_a + 1e-12);
}

// --- phase-insensitive matching -------------------------------------------
//
// A fingerprint that says *what a slice sounds like* while ignoring *when its
// waveform happens to start*: the magnitude spectrum, which throws phase away.
// Two renderings of the same drum differ wildly sample-by-sample and hardly at
// all here.

// In-place iterative radix-2 FFT. Only the magnitudes are wanted, so this stays
// as small as it can be rather than pulling in a dependency.
void fft(std::vector<float>& re, std::vector<float>& im) {
  const std::size_t n = re.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {  // bit-reversal permutation
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j |= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
    const float wr = static_cast<float>(std::cos(ang));
    const float wi = static_cast<float>(std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (std::size_t k = 0; k < len / 2; ++k) {
        const float ur = re[i + k], ui = im[i + k];
        const float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        const float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr;
        im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr;
        im[i + k + len / 2] = ui - vi;
        const float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

// Unit-length magnitude spectrum of stem[start, end), mono and Hann-windowed.
// Long slices are fingerprinted from their head: the length is already part of
// the bucket key, and what a sound *is* is settled well before a third of a
// second is out.
constexpr std::size_t kFpFrames = 16384;  // ~0.37 s at 44.1 kHz
constexpr std::size_t kFpBands = 128;

std::vector<float> fingerprint(const std::vector<float>& stem, long start, long end) {
  const std::size_t want =
      std::min<std::size_t>(static_cast<std::size_t>(end - start), kFpFrames);
  std::size_t n = 1;
  while (n < want) n <<= 1;

  std::vector<float> re(n, 0.0f), im(n, 0.0f);
  for (std::size_t i = 0; i < want; ++i) {
    const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / (want > 1 ? want - 1 : 1));
    const long f = start + static_cast<long>(i);
    re[i] = 0.5f * (stem[2 * f] + stem[2 * f + 1]) * w;
  }
  fft(re, im);

  // Fold the half-spectrum into bands spaced *logarithmically*. Linear bands
  // would put every bass fundamental into the same one -- 128 bands across
  // 22 kHz is 172 Hz wide, and a bass note is 60 Hz -- so two different bass
  // notes would look like the same sound. Log spacing keeps a constant number
  // of bands per octave, so low pitches stay as distinguishable as high ones.
  std::vector<float> fp(kFpBands, 0.0f);
  const std::size_t half = n / 2;
  const double bin_hz = 44100.0 / static_cast<double>(n);
  const double lo = 30.0, hi = 20000.0;
  const double span = std::log(hi / lo);
  for (std::size_t k = 1; k < half; ++k) {
    const double hz = k * bin_hz;
    if (hz < lo) continue;
    std::size_t b = kFpBands - 1;
    if (hz < hi)
      b = static_cast<std::size_t>(kFpBands * std::log(hz / lo) / span);
    b = std::min(b, kFpBands - 1);
    fp[b] += std::sqrt(re[k] * re[k] + im[k] * im[k]);
  }
  double norm = 0.0;
  for (float v : fp) norm += static_cast<double>(v) * v;
  norm = std::sqrt(norm);
  if (norm > 0.0)
    for (float& v : fp) v = static_cast<float>(v / norm);
  return fp;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
  double d = 0.0;
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
    d += static_cast<double>(a[i]) * b[i];
  return d;
}

// Fade a keysound's edges to zero, so it always starts and ends at silence and
// the joins between consecutive ones cannot step -- which is what makes
// substituting a phase-shifted near-match safe.
//
// The two edges do different jobs and want different lengths. The fade-*out* is
// what actually keeps a seam quiet, and can be long. The fade-*in* only has to
// get the first sample to zero, and every millisecond of it is a millisecond of
// a percussive attack being blunted -- so it wants to be as short as it can be
// while still ramping rather than stepping.
void fade_edges(std::vector<std::int16_t>& pcm, int rate, double in_ms,
                double out_ms) {
  if (pcm.empty()) return;
  const long frames = static_cast<long>(pcm.size() / 2);
  auto ramp = [&](double ms) {
    long f = static_cast<long>(ms * rate / 1000.0);
    return std::max<long>(0, std::min(f, frames / 2));
  };
  const long fin = ramp(in_ms), fout = ramp(out_ms);

  for (long i = 0; i < fin; ++i) {
    const float g = static_cast<float>(i) / static_cast<float>(fin);
    pcm[2 * i] = static_cast<std::int16_t>(std::lrintf(pcm[2 * i] * g));
    pcm[2 * i + 1] = static_cast<std::int16_t>(std::lrintf(pcm[2 * i + 1] * g));
  }
  for (long i = 0; i < fout; ++i) {
    const float g = static_cast<float>(i) / static_cast<float>(fout);
    const long j = frames - 1 - i;
    pcm[2 * j] = static_cast<std::int16_t>(std::lrintf(pcm[2 * j] * g));
    pcm[2 * j + 1] = static_cast<std::int16_t>(std::lrintf(pcm[2 * j + 1] * g));
  }
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

  // Near-duplicate matching (opt-in, see ConvertOptions::dedup_tolerance_db).
  // A keysound is remembered as a region of the stem it came from rather than a
  // copy of its audio. Candidates are bucketed by (voice, length) because only
  // equal-length slices can be compared sample-for-sample, which also keeps the
  // search short.
  struct Rep {
    long start = 0, end = 0;
    double rms = 0.0;
    int id = 0;
    std::vector<float> fp;  // magnitude spectrum, only in ignore-phase mode
  };
  const double tolerance = -std::fabs(opts.dedup_tolerance_db);
  // Phase-insensitive merging is only safe when we know what each note *is*:
  // matching on a phase-blind spectrum alone happily merges a bass note with
  // the one a tone below it, which transposes the music. Refuse rather than do
  // that. (The libgme backend has no note events, so it never gets this.)
  const bool have_ids = song.onset_ids.size() == stems.size();
  const bool ignore_phase = render && opts.dedup_ignore_phase && have_ids;
  if (render && opts.dedup_ignore_phase && !have_ids)
    report(opts,
           "ignoring --dedup-ignore-phase: this backend has no note events, and "
           "without them it cannot tell two pitches apart");
  const bool tolerant = render && (opts.dedup_tolerance_db > 0.0 || ignore_phase);
  // Two spectra this close are the same sound; below it they are not. Chosen by
  // measuring against a Genesis module -- tighter keeps near-identical drum hits
  // apart, looser starts merging different notes.
  constexpr double kSameSound = 0.995;
  // With phase thrown away, keysounds no longer join smoothly, so fade their
  // edges to zero and every seam becomes silence-to-silence.
  const double fade_ms = opts.keysound_fade_ms >= 0.0 ? opts.keysound_fade_ms
                         : ignore_phase             ? 2.0
                                                    : 0.0;
  const double attack_ms = opts.keysound_attack_ms >= 0.0
                               ? opts.keysound_attack_ms
                           : ignore_phase ? 0.3
                                          : 0.0;
  // (voice, length, note id) -- the note id is -1 unless we know it, and in
  // ignore-phase mode a merge can only ever happen inside one of these buckets.
  std::map<std::tuple<std::size_t, long, int>, std::vector<Rep>> reps;
  long merged_count = 0;

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
        const long tend = trim_end(stems[v], start, end);
        std::vector<std::int16_t> pcm = slice_pcm(stems[v], start, tend);
        ++total_slices;
        std::string raw(reinterpret_cast<const char*>(pcm.data()),
                        pcm.size() * sizeof(std::int16_t));
        auto found = dedup.find(raw);

        // Byte-identical? Done. Otherwise, if asked, look for one that merely
        // sounds the same.
        int near = -1;
        if (found == dedup.end() && tolerant) {
          const long len = tend - start;
          const double rms = slice_rms(stems[v], start, tend);
          // Same instrument, same pitch, same volume -- or, without note events,
          // no constraint beyond the audio itself (the strict path only).
          const int note_id =
              (ignore_phase && k < song.onset_ids[v].size()) ? song.onset_ids[v][k]
                                                             : -1;
          std::vector<Rep>& bucket = reps[{v, len, note_id}];
          std::vector<float> fp;
          if (ignore_phase) fp = fingerprint(stems[v], start, tend);

          if (rms > 0.0) {
            for (const Rep& r : bucket) {
              if (ignore_phase) {
                // Same timbre, whatever the phase. This is the only test that
                // can see two renderings of one drum as one sound.
                if (cosine(fp, r.fp) > kSameSound) {
                  near = r.id;
                  break;
                }
                continue;
              }
              // rms(a - b) >= |rms(a) - rms(b)|, so a big level difference
              // cannot possibly come in under the tolerance -- skip the work.
              const double floor_db =
                  20.0 * std::log10(std::fabs(rms - r.rms) / rms + 1e-12);
              if (floor_db >= tolerance) continue;
              if (residual_db(stems[v], start, stems[v], r.start, len, rms) <
                  tolerance) {
                near = r.id;
                break;
              }
            }
          }
          if (near < 0)
            bucket.push_back({start, tend, rms, static_cast<int>(channels.size()),
                              std::move(fp)});
          else
            ++merged_count;
        }

        if (near >= 0) {
          id = near;
        } else if (found == dedup.end()) {
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
          fade_edges(pcm, rate, attack_ms, fade_ms);
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

  if (render) {
    std::string msg = "wrote " + std::to_string(channels.size()) +
                      " unique keysounds from " + std::to_string(total_slices) +
                      " slices";
    if (ignore_phase)
      msg += " (" + std::to_string(merged_count) +
             " merged as the same sound, ignoring phase)";
    else if (tolerant)
      msg += " (" + std::to_string(merged_count) + " merged as near-duplicates at " +
             std::to_string(static_cast<int>(tolerance)) + " dB)";
    report(opts, msg);
  }

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
