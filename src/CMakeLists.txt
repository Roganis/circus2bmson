# Core conversion library.
#   score.cpp    format-agnostic score via libopenmpt (MOD/XM/S3M/IT/...)
#   render.cpp   per-channel keysound rendering + dedup (WAV via dr_wav,
#                OGG via libvorbis)
#   bmson.cpp    bmson document emission
#   stemconv.cpp shared back half of the audio-domain backends: stems ->
#                onsets -> keysounds -> bmson (used by chip.cpp and furnace.cpp)
#   mod.cpp / timeline.cpp   independent MOD parser + sequencer, kept as a
#                validation oracle for the generic path (see tests)
add_library(circus2bmson
  version.cpp
  mod.cpp
  timeline.cpp
  score.cpp
  midi.cpp
  midisynth.cpp
  midiplayer.cpp
  midiconv.cpp
  bmson.cpp
  render.cpp
  onset.cpp
  stemconv.cpp
  chip.cpp
  furnace.cpp
  oggenc.cpp
  dr_wav_impl.cpp
  convert.cpp
)
add_library(circus2bmson::circus2bmson ALIAS circus2bmson)

target_include_directories(circus2bmson
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(circus2bmson
  PUBLIC c2b_vendor
  PRIVATE PkgConfig::LIBOPENMPT PkgConfig::VORBISENC PkgConfig::FLUIDSYNTH)

target_compile_definitions(circus2bmson
  PRIVATE C2B_VERSION="${PROJECT_VERSION}")

# Chip backend: only real when libgme is present (chip.cpp is otherwise a stub).
if(LIBGME_FOUND)
  target_link_libraries(circus2bmson PRIVATE PkgConfig::LIBGME)
  target_compile_definitions(circus2bmson PRIVATE C2B_HAVE_LIBGME)
endif()
