# Core conversion library.
#   M1: MOD parser + timeline builder + bmson skeleton emitter.
#   M2/M3 will add keysound rendering (libopenmpt) and the final bmson output.
add_library(circus2bmson
  version.cpp
  mod.cpp
  timeline.cpp
  bmson.cpp
  convert.cpp
)
add_library(circus2bmson::circus2bmson ALIAS circus2bmson)

target_include_directories(circus2bmson
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(circus2bmson
  PUBLIC c2b_vendor)

target_compile_definitions(circus2bmson
  PRIVATE C2B_VERSION="${PROJECT_VERSION}")
