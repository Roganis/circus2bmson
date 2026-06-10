# Core conversion library.
#   M1: MOD parser + timeline builder + bmson emitter.
#   M2: per-channel keysound rendering (libopenmpt) + WAV output (dr_wav).
add_library(circus2bmson
  version.cpp
  mod.cpp
  timeline.cpp
  bmson.cpp
  render.cpp
  dr_wav_impl.cpp
  convert.cpp
)
add_library(circus2bmson::circus2bmson ALIAS circus2bmson)

target_include_directories(circus2bmson
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(circus2bmson
  PUBLIC c2b_vendor
  PRIVATE PkgConfig::LIBOPENMPT)

target_compile_definitions(circus2bmson
  PRIVATE C2B_VERSION="${PROJECT_VERSION}")
