// stem_sum_check -- M0.5 de-risking spike.
//
// circus2bmson plans to extract one keysound per note by rendering each MOD
// channel in isolation (mute all channels but one, via libopenmpt's
// interactive interface) and slicing the resulting stem at note-on times.
// That only works if isolated channel renders SUM BACK to the full mix --
// i.e. muting is a clean linear operation and global state (tempo, jumps) is
// unaffected by it. This tool measures exactly that:
//
//   1. render the full mix,
//   2. render N stems, each with only channel c audible,
//   3. sum the stems and compare against the full mix.
//
// PASS (tiny residual) validates approach #2 for keysound extraction.
//
// usage: stem_sum_check <module> [--max-peak P] [--max-rms-db DB]
//   exit 0 = PASS, 1 = FAIL (residual over limits), 2 = usage, 3 = error.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>

namespace {

constexpr std::int32_t kSampleRate = 44100;
constexpr std::size_t kChunkFrames = 4096;
// Safety cap so a pathological infinitely-looping module cannot hang the spike.
constexpr std::size_t kMaxFrames = static_cast<std::size_t>(kSampleRate) * 600;

std::vector<char> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return std::vector<char>((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
}

// Render the whole module (honouring its current mute state) to interleaved
// stereo float, playing through once.
std::vector<float> render_all(openmpt::module& mod) {
  std::vector<float> out;
  std::vector<float> chunk(2 * kChunkFrames);
  std::size_t frames = 0;
  for (;;) {
    const std::size_t n =
        mod.read_interleaved_stereo(kSampleRate, kChunkFrames, chunk.data());
    if (n == 0) break;
    out.insert(out.end(), chunk.begin(), chunk.begin() + 2 * n);
    frames += n;
    if (frames >= kMaxFrames) break;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: stem_sum_check <module> [--max-peak P] [--max-rms-db DB]\n";
    return 2;
  }
  const std::string path = argv[1];
  double max_peak = 1e-3;      // fail if any sample differs by more than this
  double max_rms_db = -80.0;   // fail if residual RMS (rel. to signal) exceeds this
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--max-peak") max_peak = std::stod(argv[i + 1]);
    else if (k == "--max-rms-db") max_rms_db = std::stod(argv[i + 1]);
  }

  try {
    const std::vector<char> data = read_file(path);

    openmpt::module_ext full(data);
    const std::int32_t nch = full.get_num_channels();
    std::cerr << "module  : " << path << "\n"
              << "title   : " << full.get_metadata("title") << "\n"
              << "type    : " << full.get_metadata("type") << " ("
              << full.get_metadata("type_long") << ")\n"
              << "channels: " << nch << "\n"
              << "samples : " << full.get_num_samples() << "\n"
              << "orders  : " << full.get_num_orders() << "\n"
              << "patterns: " << full.get_num_patterns() << "\n"
              << "duration: " << full.get_duration_seconds() << " s\n\n";

    const std::vector<float> mix = render_all(full);

    std::vector<float> summed(mix.size(), 0.0f);
    for (std::int32_t c = 0; c < nch; ++c) {
      openmpt::module_ext mod(data);
      auto* interactive = static_cast<openmpt::ext::interactive*>(
          mod.get_interface(openmpt::ext::interactive_id));
      if (!interactive)
        throw std::runtime_error("libopenmpt interactive interface unavailable");
      for (std::int32_t j = 0; j < nch; ++j)
        interactive->set_channel_mute_status(j, /*mute=*/j != c);

      const std::vector<float> stem = render_all(mod);
      if (stem.size() != mix.size())
        std::cerr << "  note: channel " << c << " stem length " << stem.size()
                  << " != mix " << mix.size() << " (comparing overlap)\n";
      const std::size_t n = std::min(summed.size(), stem.size());
      for (std::size_t i = 0; i < n; ++i) summed[i] += stem[i];
    }

    const std::size_t n = std::min(mix.size(), summed.size());
    double peak = 0.0, sq_err = 0.0, sq_sig = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double d = static_cast<double>(mix[i]) - static_cast<double>(summed[i]);
      peak = std::max(peak, std::fabs(d));
      sq_err += d * d;
      sq_sig += static_cast<double>(mix[i]) * static_cast<double>(mix[i]);
    }
    const std::size_t denom = std::max<std::size_t>(1, n);
    const double rms_err = std::sqrt(sq_err / denom);
    const double rms_sig = std::sqrt(sq_sig / denom);
    const double rel_db =
        (rms_sig > 0.0) ? 20.0 * std::log10((rms_err + 1e-30) / rms_sig)
                        : (rms_err > 0.0 ? 0.0 : -1000.0);

    std::cerr << "frames  : " << n / 2 << "\n"
              << "peak |d|: " << peak << "\n"
              << "rms err : " << rms_err << "\n"
              << "rms sig : " << rms_sig << "\n"
              << "residual: " << rel_db << " dB (relative to signal)\n";

    const bool ok = (peak <= max_peak) && (rel_db <= max_rms_db);
    std::cerr << "verdict : " << (ok ? "PASS" : "FAIL") << "  (limits: peak<="
              << max_peak << ", residual<=" << max_rms_db << " dB)\n";
    return ok ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 3;
  }
}
