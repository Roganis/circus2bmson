# Minimal Dear ImGui + GLFW front-end. Vertical slice: window + drag-drop +
# convert with default options. ImGui is vendored under third_party/imgui;
# GLFW is a system package (apt libglfw3-dev / MSYS2 mingw-w64-*-glfw).
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(circus2bmson_gui
  main.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
set_target_properties(circus2bmson_gui PROPERTIES OUTPUT_NAME circus2bmson-gui)

target_include_directories(circus2bmson_gui PRIVATE
  ${IMGUI_DIR} ${IMGUI_DIR}/backends)

target_link_libraries(circus2bmson_gui PRIVATE
  circus2bmson::circus2bmson
  PkgConfig::GLFW
  OpenGL::GL
  Threads::Threads)
