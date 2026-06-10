#include <iostream>
#include <string>

#include "circus2bmson/circus2bmson.hpp"

namespace {

void print_usage(const char* argv0) {
  std::cout << "circus2bmson " << circus2bmson::version() << "\n"
            << "Convert tracker modules (MOD first) to the bmson format.\n\n"
            << "usage:\n"
            << "  " << argv0 << " <input.mod> [-o <output_dir>]\n"
            << "  " << argv0 << " --version\n"
            << "  " << argv0 << " --help\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string input;
  std::string outdir;

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
      outdir = argv[i];
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

  (void)outdir;
  std::cerr << "circus2bmson: MOD->bmson conversion is not implemented yet (M1+).\n"
               "M0 establishes the build, dependencies and CI, and validates the\n"
               "per-channel stem-rendering approach via the 'stem_sum_check' tool.\n";
  return 1;
}
