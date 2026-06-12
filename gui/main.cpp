// Minimal circus2bmson GUI (vertical slice): pick a module (Browse or drag it
// onto the window) and convert it with default options, showing the result.
// The full option widgets and a (MIDI) soundfont picker land next; this slice
// proves the Dear ImGui + GLFW build and the CI/artifact path.
#include <atomic>
#include <cstdio>
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
  std::string input_path;
  std::mutex mtx;  // guards `log`
  std::string log;
  std::atomic<bool> busy{false};
};

AppState* g_app = nullptr;  // for the GLFW drop callback (C function pointer)

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
  if (s.busy || s.input_path.empty()) return;
  s.busy = true;
  log_line(s, "converting " + s.input_path + " ...");
  std::thread(convert_worker, &s, s.input_path).detach();
}

void pick_input(AppState& s) {
  auto sel = pfd::open_file(
                 "Select a module", ".",
                 {"Tracker modules",
                  "*.mod *.xm *.s3m *.it *.mptm *.mtm *.669 *.med *.okt *.dbm "
                  "*.ptm *.stm *.ult *.far",
                  "All files", "*"})
                 .result();
  if (!sel.empty()) s.input_path = sel[0];
}

void drop_callback(GLFWwindow*, int count, const char** paths) {
  if (g_app && count > 0) g_app->input_path = paths[0];
}

void glfw_error(int code, const char* desc) {
  std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

}  // namespace

int main() {
  glfwSetErrorCallback(glfw_error);

#if defined(__linux__) && defined(GLFW_PLATFORM_X11)
  // GLFW delivers file-drop events only on its X11 backend, not on Wayland, so
  // prefer X11 (XWayland under a Wayland session) and drag-and-drop works.
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

  if (!glfwInit()) {
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
      glfwCreateWindow(680, 460, "circus2bmson", nullptr, nullptr);
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
    ImGui::TextUnformatted("Choose a module, or drag one onto the window.");
    if (ImGui::Button("Browse...")) pick_input(state);
    ImGui::SameLine();
    ImGui::Text("input: %s",
                state.input_path.empty() ? "(none)" : state.input_path.c_str());

    ImGui::BeginDisabled(state.busy || state.input_path.empty());
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
