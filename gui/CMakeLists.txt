# Minimal Dear ImGui + GLFW front-end. Vertical slice: window + drag-drop +
# convert with default options. ImGui is vendored under third_party/imgui;
# GLFW is a system package (apt libglfw3-dev / MSYS2 mingw-w64-*-glfw).
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(circus2bmson_gui
  main.cpp
  audio.cpp                       # miniaudio playback device (live MIDI preview)
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
set_target_properties(circus2bmson_gui PROPERTIES OUTPUT_NAME circus2bmson-gui)

# Vendored miniaudio trips -Wunused-result in its ALSA backend; not our code.
if(NOT MSVC)
  set_source_files_properties(audio.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-result")
endif()

target_include_directories(circus2bmson_gui PRIVATE
  ${IMGUI_DIR} ${IMGUI_DIR}/backends)

target_link_libraries(circus2bmson_gui PRIVATE
  circus2bmson::circus2bmson      # also brings the vendored third_party headers
  PkgConfig::GLFW
  OpenGL::GL
  Threads::Threads)

# miniaudio's platform audio backends.
if(WIN32)
  target_link_libraries(circus2bmson_gui PRIVATE ole32 winmm)
elseif(APPLE)
  target_link_libraries(circus2bmson_gui PRIVATE
    "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
else()
  target_link_libraries(circus2bmson_gui PRIVATE ${CMAKE_DL_LIBS} m)
endif()
