# Core conversion library. At M0 this is a thin stub; the MOD loader, timeline
# builder, keysound renderer and bmson emitter land in M1-M3.
add_library(circus2bmson
  version.cpp
)
add_library(circus2bmson::circus2bmson ALIAS circus2bmson)

target_include_directories(circus2bmson
  PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(circus2bmson
  PUBLIC c2b_vendor)

target_compile_definitions(circus2bmson
  PRIVATE C2B_VERSION="${PROJECT_VERSION}")
