#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "circus2bmson/circus2bmson.hpp"
#include "circus2bmson/convert.hpp"

namespace {

void print_usage(const char* argv0) {
  std::cout
      << "circus2bmson " << circus2bmson::version() << "\n"
      << "Convert tracker modules (and MIDI) to the bmson format.\n"
      << "Accepts any format libopenmpt plays (MOD, XM, S3M, IT, ...) plus MIDI;\n"
      << "see --list-formats.\n\n"
      << "usage:\n"
      << "  " << argv0 << " <module> [-o <output_dir>] [options]\n"
      << "  " << argv0 << " --version\n"
      << "  " << argv0 << " --help\n"
      << "  " << argv0 << " --list-formats\n\n"
      << "options:\n"
      << "  -o, --output DIR    output folder (default: a folder named after the input)\n"
      << "  --format FMT        keysound files: wav (default) | ogg\n"
      << "  --soundfont FILE    SoundFont (.sf2) for MIDI input\n"
      << "  --furnace FILE      Furnace binary, to render .dmf/.fur input\n"
      << "  --max-loops N       times to unroll a looping section (default 1)\n"
      << "  --name-by WHICH     keysound names: channel (default) | instrument | lane\n"
      << "  --volume-ramping    keep libopenmpt's anti-click ramp (more keysounds)\n"
      << "  --no-audio          emit bmson skeleton only (no keysound files)\n"
      << "  --list-formats      list the input extensions this build accepts\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string input;
  circus2bmson::ConvertOptions opts;
  bool output_given = false;

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
    if (a == "--list-formats") {
      const auto exts = circus2bmson::supported_input_extensions();
      for (std::size_t k = 0; k < exts.size(); ++k)
        std::cout << exts[k] << (k + 1 < exts.size() ? " " : "\n");
      return 0;
    }
    if (a == "-o" || a == "--output") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      opts.output_dir = argv[i];
      output_given = true;
    } else if (a == "--format") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      const std::string fmt = argv[i];
      if (fmt == "wav") {
        opts.audio_format = circus2bmson::AudioFormat::Wav;
      } else if (fmt == "ogg") {
        opts.audio_format = circus2bmson::AudioFormat::Ogg;
      } else {
        std::cerr << "error: --format expects 'wav' or 'ogg'\n";
        return 2;
      }
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
    } else if (a == "--soundfont") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      opts.soundfont_path = argv[i];
    } else if (a == "--furnace") {
      if (++i >= argc) {
        std::cerr << "error: missing argument for " << a << "\n";
        return 2;
      }
      opts.furnace_path = argv[i];
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

  // With no -o (e.g. dragging a module onto the executable), write to a folder
  // named after the module, beside it.
  if (!output_given) {
    const std::filesystem::path in(input);
    opts.output_dir = (in.parent_path() / in.stem()).string();
  }

  // Chip conversions take minutes; say what is happening. On stderr so the
  // result summary on stdout stays pipeable.
  opts.on_progress = [](const std::string& m) { std::cerr << m << "\n"; };

  try {
    const circus2bmson::ConvertResult r =
        circus2bmson::convert_mod_file(input, opts);
    std::cout << "wrote   : " << r.bmson_path << "\n"
              << "title   : " << r.title << "\n"
              << "format  : " << r.format << "\n"
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
    if (r.coarse_rows > 0)
      std::cout << "warning : " << r.coarse_rows
                << " row onset(s) could not be pinned exactly\n";
    if (r.truncated)
      std::cout << "warning : playback hit the 30-minute safety cap (truncated)\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
