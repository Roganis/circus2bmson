cmake_minimum_required(VERSION 3.16)
project(circus2bmson
  VERSION 0.0.1
  DESCRIPTION "Convert tracker modules (MOD first) to the bmson format"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(C2B_BUILD_CLI   "Build the circus2bmson CLI"            ON)
option(C2B_BUILD_TOOLS "Build diagnostic / spike tools"        ON)
option(C2B_BUILD_TESTS "Enable tests (ctest)"                  ON)
# OFF by default so the headless library/CLI build (and BmsTWO embedding) stay
# free of GUI dependencies; enable with -DC2B_BUILD_GUI=ON (needs GLFW).
option(C2B_BUILD_GUI   "Build the circus2bmson GUI (Dear ImGui)" OFF)

# ---------------------------------------------------------------------------
# Dependencies
#   libopenmpt : system package (libopenmpt-dev), located via pkg-config.
#   nlohmann/json, dr_wav : vendored single headers under third_party/.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOPENMPT REQUIRED IMPORTED_TARGET libopenmpt)
pkg_check_modules(VORBISENC REQUIRED IMPORTED_TARGET vorbisenc vorbis ogg)

add_library(c2b_vendor INTERFACE)
target_include_directories(c2b_vendor INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(C2B_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(C2B_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(C2B_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(C2B_BUILD_GUI)
  add_subdirectory(gui)
endif()

message(STATUS "circus2bmson ${PROJECT_VERSION}  (libopenmpt ${LIBOPENMPT_VERSION})")
