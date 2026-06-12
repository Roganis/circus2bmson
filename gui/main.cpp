// Minimal circus2bmson GUI (vertical slice): point it at a module (type/paste a
// path, Browse, or drag one onto the window) and convert it with default
// options. The path field works with no external deps; Browse needs a system
// dialog helper (zenity/kdialog); drag-drop needs GLFW's X11 backend. Full
// option widgets and a (MIDI) soundfont picker land next.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable_file_dialogs/portable-file-dialogs.h"

#include "circus2bmson/circus2bmson.hpp"
#include "circus2bmson/convert.hpp"

namespace {

struct AppState {
  char input[1024] = {0};  // module path (only touched on the main thread)
  std::mutex mtx;          // guards `log`
  std::string log;
  std::atomic<bool> busy{false};
};

AppState* g_app = nullptr;  // for the GLFW drop callback (C function pointer)

void set_input(AppState& s, const std::string& path) {
  std::snprintf(s.input, sizeof(s.input), "%s", path.c_str());
}

void log_line(AppState& s, const std::string& line) {
  std::lock_guard<std::mutex> lk(s.mtx);
  s.log += line;
  s.log.push_back('\n');
}

// Mirror the CLI's default: a folder named after the input, beside it.
std::string default_output_dir(const std::string& input) {
  const std::filesystem::path in(input);
  return (in.parent_path() / in.stem()).string();
}

void convert_worker(AppState* s, std::string input) {
  try {
    circus2bmson::ConvertOptions opts;
    opts.output_dir = default_output_dir(input);
    const circus2bmson::ConvertResult r =
        circus2bmson::convert_mod_file(input, opts);
    std::string m;
    m += "wrote   : " + r.bmson_path + "\n";
    m += "title   : " + r.title + "\n";
    m += "format  : " + r.format + "\n";
    m += "channels: " + std::to_string(r.channels) + "\n";
    m += "notes   : " + std::to_string(r.note_count);
    if (r.audio_rendered)
      m += "\nkeysound: " + std::to_string(r.keysound_count) + " unique";
    log_line(*s, m);
  } catch (const std::exception& e) {
    log_line(*s, std::string("error: ") + e.what());
  }
  s->busy = false;
}

void start_convert(AppState& s) {
  if (s.busy || s.input[0] == '\0') return;
  s.busy = true;
  std::string in = s.input;
  log_line(s, "converting " + in + " ...");
  std::thread(convert_worker, &s, std::move(in)).detach();
}

void pick_input(AppState& s) {
  auto sel = pfd::open_file(
                 "Select a module", ".",
                 {"Tracker modules",
                  "*.mod *.xm *.s3m *.it *.mptm *.mtm *.669 *.med *.okt *.dbm "
                  "*.ptm *.stm *.ult *.far",
                  "All files", "*"})
                 .result();
  if (!sel.empty()) set_input(s, sel[0]);
}

void drop_callback(GLFWwindow*, int count, const char** paths) {
  if (g_app && count > 0) set_input(*g_app, paths[0]);
}

void glfw_error(int code, const char* desc) {
  std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

bool init_glfw() {
#if defined(__linux__) && defined(GLFW_PLATFORM_X11)
  // Prefer X11 (drag-drop only works on GLFW's X11 backend; under Wayland it
  // runs via XWayland). Fall back to the default platform if X11 is
  // unavailable -- e.g. a Wayland session with no XWayland -- so the app still
  // starts (the path field and Browse still work there).
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
  if (glfwInit()) return true;
  glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
#endif
  return glfwInit();
}

}  // namespace

int main() {
  glfwSetErrorCallback(glfw_error);
  if (!init_glfw()) {
    std::fprintf(stderr, "failed to initialise GLFW\n");
    return 1;
  }

#if defined(__APPLE__)
  const char* glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
  const char* glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow* window =
      glfwCreateWindow(700, 480, "circus2bmson", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "failed to create window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  AppState state;
  g_app = &state;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);
  glfwSetDropCallback(window, drop_callback);  // after ImGui installs its own

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("circus2bmson");
    ImGui::TextUnformatted("Module path (type/paste, Browse, or drag a file in):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##path", "/path/to/song.mod", state.input,
                             sizeof(state.input));
    if (ImGui::Button("Browse...")) pick_input(state);
    ImGui::SameLine();
    ImGui::BeginDisabled(state.busy || state.input[0] == '\0');
    if (ImGui::Button("Convert")) start_convert(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(state.busy ? "converting..." : "");

    ImGui::Separator();
    ImGui::TextUnformatted("Log:");
    ImGui::BeginChild("log", ImVec2(0, 0));
    {
      std::lock_guard<std::mutex> lk(state.mtx);
      ImGui::TextUnformatted(state.log.c_str());
    }
    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
