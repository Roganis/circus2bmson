#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "circus2bmson/circus2bmson.hpp"
#include "circus2bmson/convert.hpp"

namespace {

void print_usage(const char* argv0) {
  std::cout << "circus2bmson " << circus2bmson::version() << "\n"
            << "Convert tracker modules (MOD first) to the bmson format.\n\n"
            << "usage:\n"
            << "  " << argv0 << " <input.mod> [-o <output_dir>] [--max-loops N]\n"
            << "  " << argv0 << " --version\n"
            << "  " << argv0 << " --help\n\n"
            << "options:\n"
            << "  -o, --output DIR    output folder (default: current dir)\n"
            << "  --max-loops N       times to unroll a looping section (default 1)\n"
            << "  --name-by WHICH     keysound names: channel (default) | instrument | lane\n"
            << "  --volume-ramping    keep libopenmpt's anti-click ramp (more keysounds)\n"
            << "  --no-audio          emit bmson skeleton only (no keysound WAVs)\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string input;
  circus2bmson::ConvertOptions opts;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--version" || a == "-v") {
      std::cout << circus2bmson::version() << "\n";
      return 0;
    }
    if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if (a == "-o" || a == "--output") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      opts.output_dir = argv[i];
    } else if (a == "--max-loops") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      opts.max_loops = std::atoi(argv[i]);
    } else if (a == "--name-by") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      const std::string which = argv[i];
      if (which == "channel") {
        opts.keysound_naming = circus2bmson::KeysoundNaming::Channel;
      } else if (which == "instrument") {
        opts.keysound_naming = circus2bmson::KeysoundNaming::Instrument;
      } else if (which == "lane") {
        opts.keysound_naming = circus2bmson::KeysoundNaming::Lane;
      } else {
        std::cerr << "error: --name-by expects 'channel', 'instrument' or 'lane'\n";
        return 2;
      }
    } else if (a == "--volume-ramping") {
      opts.volume_ramping = true;
    } else if (a == "--no-audio") {
      opts.render_audio = false;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "error: unknown option: " << a << "\n";
      return 2;
    } else {
      input = a;
    }
  }

  if (input.empty()) {
    print_usage(argv[0]);
    return 2;
  }

  try {
    const circus2bmson::ConvertResult r =
        circus2bmson::convert_mod_file(input, opts);
    std::cout << "wrote   : " << r.bmson_path << "\n"
              << "title   : " << r.title << "\n"
              << "channels: " << r.channels << "\n"
              << "init_bpm: " << r.init_bpm << "\n"
              << "rows    : " << r.emitted_rows << "  (" << r.total_pulses
              << " pulses, " << r.total_seconds << " s)\n"
              << "notes   : " << r.note_count << "\n"
              << "bpm_evts: " << r.bpm_event_count << "\n"
              << "lines   : " << r.line_count << "\n";
    if (r.audio_rendered)
      std::cout << "keysound: " << r.keysound_count << " unique  ("
                << r.total_slices << " note slices)\n";
    if (r.missing_keysounds > 0)
      std::cout << "warning : " << r.missing_keysounds
                << " note(s) had no rendered keysound (alignment)\n";
    if (r.loops_played > 0)
      std::cout << "loops   : " << r.loops_played << " unrolled\n";
    if (r.truncated)
      std::cout << "warning : timeline hit the safety row cap (truncated)\n";
    if (r.unsupported_flow > 0)
      std::cout << "warning : " << r.unsupported_flow
                << " unmodelled control-flow effect(s) (e.g. EEx) ignored\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
