#ifndef CIRCUS2BMSON_MIDISYNTH_HPP
#define CIRCUS2BMSON_MIDISYNTH_HPP

#include <string>
#include <vector>

// FluidSynth wrapper that renders one MIDI note at a time in isolation, so each
// note becomes a keysound. MIDI carries no audio of its own, so the timbres
// come entirely from the loaded SoundFont.
namespace circus2bmson {

class MidiSynth {
 public:
  MidiSynth(const std::string& soundfont_path, int sample_rate);
  ~MidiSynth();
  MidiSynth(const MidiSynth&) = delete;
  MidiSynth& operator=(const MidiSynth&) = delete;

  bool ok() const { return synth_ != nullptr && sfid_ >= 0; }

  // Render one note to interleaved stereo float: the note held for
  // `duration_frames`, then its release tail until it decays. Drums use the
  // GM percussion bank (key selects the instrument). `volume`/`expression`/`pan`
  // are the channel controllers (CC7/CC11/CC10) applied before the note sounds;
  // `gain` is an extra linear multiplier baked into the result (the user mix).
  std::vector<float> render_note(int program, int key, int velocity, bool drum,
                                 long duration_frames, int volume = 100,
                                 int expression = 127, int pan = 64,
                                 float gain = 1.0f);

 private:
  void* settings_ = nullptr;  // fluid_settings_t*
  void* synth_ = nullptr;     // fluid_synth_t*
  int sfid_ = -1;
  int rate_ = 44100;
};

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_MIDISYNTH_HPP
