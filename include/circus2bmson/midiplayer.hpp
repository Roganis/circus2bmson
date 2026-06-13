#ifndef CIRCUS2BMSON_MIDIPLAYER_HPP
#define CIRCUS2BMSON_MIDIPLAYER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "circus2bmson/midimix.hpp"

// Real-time MIDI auditioning for the GUI mixer. Loads a SoundFont + a MIDI file
// and renders the song on demand (pull model) so a host audio device can play
// it, while exposing per-channel / per-instrument live levels and adjustable
// gains. Public methods are for the UI thread; render() runs on the audio
// thread. FluidSynth and miniaudio never appear in this header.
namespace circus2bmson {

class MidiPlayer {
 public:
  // A mixer row: one MIDI channel, or one instrument when `channel < 0`.
  struct Track {
    int channel = -1;   // MIDI channel 0..15, or -1 for an instrument row
    int program = 0;    // GM program, or kDrumProgram for percussion
    bool drum = false;
    std::string name;   // label, e.g. "Ch 1 — Acoustic Grand Piano"
    long notes = 0;     // note count, for display
  };

  // Throws std::runtime_error if the SoundFont or MIDI fails to load.
  MidiPlayer(const std::string& soundfont_path,
             const std::vector<std::uint8_t>& midi_bytes, int sample_rate);
  ~MidiPlayer();
  MidiPlayer(const MidiPlayer&) = delete;
  MidiPlayer& operator=(const MidiPlayer&) = delete;

  bool ok() const;
  int sample_rate() const;

  // Mixer rows, built at load and immutable thereafter.
  const std::vector<Track>& channels() const;     // MIDI channels with notes
  const std::vector<Track>& instruments() const;  // distinct instruments used

  // Transport (UI thread).
  void play();
  void stop();
  void toggle();
  bool playing() const;
  void seek(double seconds);
  double position() const;  // seconds
  double duration() const;  // seconds

  // Mixer (UI thread). Gains are linear; 1.0 == unity.
  void set_channel_gain(int channel, float gain);
  float channel_gain(int channel) const;
  void set_instrument_gain(int program, float gain);
  float instrument_gain(int program) const;
  void set_honor_cc(bool on);
  bool honor_cc() const;
  MidiMix snapshot() const;  // current mix, to hand to the converter

  // Smoothed peak levels in 0..1 (post-gain). UI thread.
  float channel_level(int channel) const;
  float instrument_level(int program) const;

  // Render `frames` interleaved stereo frames into `out`. Audio thread only.
  void render(float* out, int frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace circus2bmson

#endif  // CIRCUS2BMSON_MIDIPLAYER_HPP
