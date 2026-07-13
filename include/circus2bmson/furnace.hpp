#ifndef CIRCUS2BMSON_FURNACE_HPP
#define CIRCUS2BMSON_FURNACE_HPP

#include <string>
#include <vector>

// Furnace backend: DefleMask (.dmf) and Furnace (.fur) modules.
//
// These are chiptune tracker formats, so playing them needs YM2612 / SN76489 /
// NES APU / SID / ... emulation, which we do not have and will not write. The
// Furnace tracker does, and its headless export renders each chip channel in
// isolation:
//
//   furnace -output out.wav -outmode perchan song.dmf   ->  out_c01.wav, ...
//
// That is the same mute-all-but-one stem trick render.cpp does with libopenmpt
// and chip.cpp does with libgme, just in another process -- so once the stems
// are back the shared stemconv.hpp path takes over.
//
// Furnace is a *runtime* dependency, found on PATH (or via $C2B_FURNACE, or
// --furnace): it is GPL-2.0-or-later with no library target, so we invoke the
// binary rather than link it, which keeps this project's licensing free of it.
//
// NOTE (step 1): notes are recovered from the rendered audio by onset detection
// (onset.hpp), *not* from the module's pattern data -- so this currently has the
// same fidelity caveats as the chip backend (legato lines are missed, BPM is
// inferred). Parsing .dmf natively for an exact score is the next step; .fur is
// a separate, unrelated format and stays on the inferred path.
namespace circus2bmson {

// Lower-case extensions this backend handles: {"dmf", "fur"}. Not conditional
// on anything at build time -- Furnace is resolved when a conversion runs.
std::vector<std::string> furnace_extensions();

// True if `ext` (with or without a leading dot, any case) is one of the above.
bool furnace_handles_extension(const std::string& ext);

// Locate the Furnace binary: `given` if non-empty (must exist), else
// $C2B_FURNACE, else "furnace" on PATH. Throws std::runtime_error with an
// install hint if none is found.
std::string resolve_furnace(const std::string& given);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_FURNACE_HPP
