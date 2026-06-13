// circus2bmson GUI: point it at a module (type/paste a path, Browse, or drag one
// onto the window), set the conversion options, and Convert. Wraps the same
// convert_mod_file the CLI uses; conversion runs on a worker thread so the
// window stays responsive.
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable_file_dialogs/portable-file-dialogs.h"

#include "circus2bmson/circus2bmson.hpp"
#include "circus2bmson/convert.hpp"
#include "circus2bmson/midiplayer.hpp"

#include "audio.hpp"

namespace {

struct AppState {
  // Widget-backed values (only touched on the main/UI thread).
  char input[1024] = {0};
  char output[1024] = {0};    // blank -> a folder named after the input
  char soundfont[1024] = {0};
  bool render_audio = true;
  int audio_format = 0;       // 0 = WAV, 1 = OGG
  int naming = 0;             // 0 = Channel, 1 = Instrument, 2 = Lane
  int max_loops = 1;
  bool volume_ramping = false;
  bool honor_cc = true;       // apply the MIDI's own CC7/CC11 volume

  // MIDI preview / mixer (live audition). Only the UI thread touches these; the
  // device pulls audio from `player` on its own thread and is always torn down
  // before `player` is reset.
  std::unique_ptr<circus2bmson::MidiPlayer> player;
  AudioDevice* device = nullptr;
  std::string loaded_path;    // input path the current player was built from
  std::string preview_status;

  std::mutex mtx;             // guards `log`
  std::string log;
  std::atomic<bool> busy{false};
};

AppState* g_app = nullptr;  // for the GLFW drop callback (C function pointer)

void copy_to(char* buf, std::size_t n, const std::string& v) {
  std::snprintf(buf, n, "%s", v.c_str());
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

bool is_midi_path(const std::string& p) {
  const auto dot = p.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = p.substr(dot);
  for (char& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
  return ext == ".mid" || ext == ".midi";
}

void stop_preview(AppState& s) {
  audio_close(s.device);  // stops the audio thread before the player is freed
  s.device = nullptr;
  s.player.reset();
  s.loaded_path.clear();
}

// Load the current input as a MIDI, build the player + open the audio device.
void load_preview(AppState& s) {
  stop_preview(s);
  try {
    std::ifstream f(s.input, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    const std::string sf = circus2bmson::resolve_soundfont(s.soundfont);
    s.player = std::make_unique<circus2bmson::MidiPlayer>(sf, bytes, 44100);
    s.player->set_honor_cc(s.honor_cc);
    s.device = audio_open(s.player.get());
    if (!s.device) throw std::runtime_error("no audio output device available");
    s.loaded_path = s.input;
    s.preview_status = std::to_string(s.player->channels().size()) +
                       " channels, " +
                       std::to_string(s.player->instruments().size()) +
                       " instruments";
  } catch (const std::exception& e) {
    stop_preview(s);
    s.preview_status = std::string("error: ") + e.what();
  }
}

// One mixer row: label, live meter, gain slider. Channel row when channel >= 0.
void mixer_row(AppState& s, const circus2bmson::MidiPlayer::Track& t) {
  const bool is_chan = t.channel >= 0;
  ImGui::PushID(is_chan ? t.channel : 1000 + t.program);
  const float lvl = is_chan ? s.player->channel_level(t.channel)
                            : s.player->instrument_level(t.program);
  ImGui::TextUnformatted(t.name.c_str());
  ImGui::SameLine(250.0f);
  ImGui::ProgressBar(lvl, ImVec2(110, 0), "");
  ImGui::SameLine();
  float g = is_chan ? s.player->channel_gain(t.channel)
                    : s.player->instrument_gain(t.program);
  ImGui::SetNextItemWidth(150.0f);
  if (ImGui::SliderFloat("##gain", &g, 0.0f, 2.0f, "%.2fx")) {
    if (is_chan)
      s.player->set_channel_gain(t.channel, g);
    else
      s.player->set_instrument_gain(t.program, g);
  }
  ImGui::PopID();
}

void convert_worker(AppState* s, std::string input,
                    circus2bmson::ConvertOptions opts) {
  try {
    const circus2bmson::ConvertResult r =
        circus2bmson::convert_mod_file(input, opts);
    std::string m;
    m += "wrote   : " + r.bmson_path + "\n";
    m += "title   : " + r.title + "\n";
    m += "format  : " + r.format + "\n";
    m += "channels: " + std::to_string(r.channels) + "\n";
    m += "notes   : " + std::to_string(r.note_count);
    if (r.audio_rendered)
      m += "\nkeysound: " + std::to_string(r.keysound_count) + " unique (" +
           std::to_string(r.total_slices) + " slices)";
    if (r.coarse_rows > 0)
      m += "\nwarning : " + std::to_string(r.coarse_rows) +
           " row onset(s) not pinned exactly";
    log_line(*s, m);
  } catch (const std::exception& e) {
    log_line(*s, std::string("error: ") + e.what());
  }
  s->busy = false;
}

void start_convert(AppState& s) {
  if (s.busy || s.input[0] == '\0') return;
  using circus2bmson::AudioFormat;
  using circus2bmson::KeysoundNaming;

  circus2bmson::ConvertOptions opts;
  opts.output_dir =
      s.output[0] != '\0' ? std::string(s.output) : default_output_dir(s.input);
  opts.render_audio = s.render_audio;
  opts.audio_format = s.audio_format == 1 ? AudioFormat::Ogg : AudioFormat::Wav;
  opts.keysound_naming = s.naming == 1   ? KeysoundNaming::Instrument
                         : s.naming == 2 ? KeysoundNaming::Lane
                                         : KeysoundNaming::Channel;
  opts.max_loops = s.max_loops < 1 ? 1 : s.max_loops;
  opts.volume_ramping = s.volume_ramping;
  opts.soundfont_path = s.soundfont;  // used for MIDI input
  // Bake the live mixer settings into the keysounds; otherwise just honour the
  // file's own volume/expression by default.
  if (s.player)
    opts.midi_mix = s.player->snapshot();
  else
    opts.midi_mix.honor_cc = s.honor_cc;

  s.busy = true;
  log_line(s, "converting " + std::string(s.input) + " ...");
  std::thread(convert_worker, &s, std::string(s.input), std::move(opts)).detach();
}

void pick_input(AppState& s) {
  // Advertise exactly what this build can read (libopenmpt formats + MIDI).
  std::string patterns;
  for (const std::string& e : circus2bmson::supported_input_extensions()) {
    if (!patterns.empty()) patterns += ' ';
    patterns += "*." + e;
  }
  auto sel = pfd::open_file("Select a module or MIDI", ".",
                            {"Modules & MIDI", patterns, "All files", "*"})
                 .result();
  if (!sel.empty()) copy_to(s.input, sizeof(s.input), sel[0]);
}

void pick_output(AppState& s) {
  auto dir = pfd::select_folder("Output folder").result();
  if (!dir.empty()) copy_to(s.output, sizeof(s.output), dir);
}

void pick_soundfont(AppState& s) {
  auto sel =
      pfd::open_file("Select a SoundFont", ".",
                     {"SoundFont", "*.sf2 *.sf3", "All files", "*"})
          .result();
  if (!sel.empty()) copy_to(s.soundfont, sizeof(s.soundfont), sel[0]);
}

void drop_callback(GLFWwindow*, int count, const char** paths) {
  if (g_app && count > 0) copy_to(g_app->input, sizeof(g_app->input), paths[0]);
}

void glfw_error(int code, const char* desc) {
  std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

// A "[path field..............] [Browse...]" row. Returns true if Browse clicked.
bool path_row(const char* id, const char* hint, char* buf, std::size_t n) {
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 92.0f);
  ImGui::InputTextWithHint(id, hint, buf, n);
  ImGui::SameLine();
  char btn[40];
  std::snprintf(btn, sizeof(btn), "Browse...%s", id);
  return ImGui::Button(btn);
}

bool init_glfw() {
#if defined(__linux__) && defined(GLFW_PLATFORM_X11)
  // Prefer X11 (drag-drop only works on GLFW's X11 backend; under Wayland it
  // runs via XWayland). Fall back to the default platform if X11 is
  // unavailable (e.g. a Wayland session with no XWayland) so the app still
  // starts and stays usable via the path field.
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
  if (glfwInit()) return true;
  glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
#endif
  return glfwInit();
}

void draw_ui(AppState& s) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("circus2bmson", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGui::TextUnformatted("Module (type/paste a path, Browse, or drag a file in):");
  if (path_row("##in", "/path/to/song.mod", s.input, sizeof(s.input)))
    pick_input(s);

  ImGui::TextUnformatted("Output folder (blank = a folder beside the input):");
  if (path_row("##out", "(default)", s.output, sizeof(s.output))) pick_output(s);

  ImGui::Dummy(ImVec2(0, 4));
  ImGui::Checkbox("Render keysounds (audio)", &s.render_audio);
  ImGui::Indent();
  ImGui::BeginDisabled(!s.render_audio);
  ImGui::TextUnformatted("Format:");
  ImGui::SameLine();
  ImGui::RadioButton("WAV", &s.audio_format, 0);
  ImGui::SameLine();
  ImGui::RadioButton("OGG", &s.audio_format, 1);
  ImGui::SetNextItemWidth(180);
  ImGui::Combo("Keysound names", &s.naming, "Channel\0Instrument\0Lane\0");
  ImGui::Checkbox("Volume ramping (libopenmpt smoothing; more keysounds)",
                  &s.volume_ramping);
  ImGui::EndDisabled();
  ImGui::Unindent();

  ImGui::SetNextItemWidth(120);
  ImGui::InputInt("Max loops (unroll looping songs)", &s.max_loops);
  if (s.max_loops < 1) s.max_loops = 1;

  ImGui::Dummy(ImVec2(0, 4));
  ImGui::TextUnformatted("SoundFont (.sf2):");
  ImGui::SameLine();
  ImGui::TextDisabled("(for MIDI input)");
  if (path_row("##sf", "(bundled default)", s.soundfont, sizeof(s.soundfont)))
    pick_soundfont(s);

  // --- MIDI preview / mixer (MIDI input only) ---
  if (is_midi_path(s.input)) {
    if (s.player && s.loaded_path != s.input) stop_preview(s);  // file changed

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SeparatorText("MIDI preview / mixer");
    if (!s.player) {
      if (ImGui::Button("Load preview")) load_preview(s);
      ImGui::SameLine();
      ImGui::TextDisabled("audition and balance channels / instruments");
      if (!s.preview_status.empty())
        ImGui::TextUnformatted(s.preview_status.c_str());
    } else {
      if (ImGui::Button(s.player->playing() ? "Stop" : "Play")) s.player->toggle();
      ImGui::SameLine();
      if (ImGui::Button("Reload")) load_preview(s);
      ImGui::SameLine();
      const double dur = s.player->duration();
      const double pos = s.player->position();
      float frac = dur > 0.0 ? static_cast<float>(pos / dur) : 0.0f;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);
      if (ImGui::SliderFloat("##seek", &frac, 0.0f, 1.0f, "")) {
        s.player->seek(static_cast<double>(frac) * dur);
      }
      ImGui::SameLine();
      ImGui::Text("%.1f / %.1f s", pos, dur);

      if (ImGui::Checkbox("Honor file volume/expression (CC7/CC11)", &s.honor_cc))
        s.player->set_honor_cc(s.honor_cc);
      ImGui::TextDisabled("Slider gains apply live and bake into Convert.");

      ImGui::BeginChild("mixer", ImVec2(0, 200), true);
      ImGui::SeparatorText("Channels");
      for (const auto& t : s.player->channels()) mixer_row(s, t);
      if (!s.player->instruments().empty()) {
        ImGui::SeparatorText("Instruments");
        for (const auto& t : s.player->instruments()) mixer_row(s, t);
      }
      ImGui::EndChild();
    }
  } else if (s.player) {
    stop_preview(s);  // input switched away from MIDI
  }

  ImGui::Dummy(ImVec2(0, 6));
  ImGui::BeginDisabled(s.busy || s.input[0] == '\0');
  if (ImGui::Button("Convert", ImVec2(120, 0))) start_convert(s);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextUnformatted(s.busy ? "converting..." : "");

  ImGui::SeparatorText("Log");
  ImGui::BeginChild("log", ImVec2(0, 0));
  {
    std::lock_guard<std::mutex> lk(s.mtx);
    ImGui::TextUnformatted(s.log.c_str());
  }
  ImGui::EndChild();
  ImGui::End();
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
      glfwCreateWindow(720, 660, "circus2bmson", nullptr, nullptr);
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
  ImGui::GetIO().IniFilename = nullptr;  // don't write imgui.ini
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);
  glfwSetDropCallback(window, drop_callback);  // after ImGui installs its own

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw_ui(state);

    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  stop_preview(state);  // stop the audio thread before tearing down

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
