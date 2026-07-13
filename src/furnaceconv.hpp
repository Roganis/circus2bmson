#ifndef CIRCUS2BMSON_FURNACECONV_HPP
#define CIRCUS2BMSON_FURNACECONV_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "circus2bmson/convert.hpp"

// Furnace (.dmf / .fur) -> bmson, mirroring the internal chipconv.hpp seam.
// Declared here so convert.cpp can dispatch to it; implemented in furnace.cpp.
namespace circus2bmson {

// Render the module's channels to stems with the Furnace binary, then hand them
// to the shared stem path (onsets -> keysounds -> bmson). `bytes` is unused --
// Furnace reads the file itself -- but kept for symmetry with the other
// backends' seams. Throws std::runtime_error if Furnace cannot be found or the
// export fails.
ConvertResult convert_furnace(const std::vector<std::uint8_t>& bytes,
                              const std::string& input_path,
                              const ConvertOptions& opts);

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_FURNACECONV_HPP
