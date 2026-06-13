#include "circus2bmson/midiplayer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <fluidsynth.h>

#include "circus2bmson/midi.hpp"

namespace circus2bmson {
namespace {

constexpr int kGroups = 16;           // one stereo group per MIDI channel
constexpr int kChunk = 512;           // event-scheduling granularity (frames)
constexpr float kMeterDecay = 0.80f;  // per-chunk meter fall-off

void silent_log(int, const char*, void*) {}  // swallow the SDL probe warning

const char* gm_name(int program) {
  static const char* k[128] = {
      "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano",
      "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord",
      "Clavi", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
      "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ",
      "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ",
      "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)",
      "Acoustic Guitar (steel)", "Electric Guitar (jazz)",
      "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar",
      "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass",
      "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
      "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin",
      "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings",
      "Orchestral Harp", "Timpani", "String Ensemble 1", "String Ensemble 2",
      "SynthStrings 1", "SynthStrings 2", "Choir Aahs", "Voice Oohs",
      "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba",
      "Muted Trumpet", "French Horn", "Brass Section", "SynthBrass 1",
      "SynthBrass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
      "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute",
      "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle",
      "Ocarina", "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
      "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)",
      "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)",
      "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
      "Pad 7 (halo)", "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)",
      "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
      "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo",
      "Shamisen", "Koto", "Kalimba", "Bag pipe", "Fiddle", "Shanai",
      "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum",
      "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise",
      "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter",
      "Applause", "Gunshot"};
  if (program == kDrumProgram) return "Drum Kit";
  return (program >= 0 && program < 128) ? k[program] : "Instrument";
}

struct Ev {
  long frame;
  int type;  // 0 = note off, 1 = note on
  int ch, key, vel, program, vol, expr, pan;
  bool drum;
};

}  // namespace

struct MidiPlayer::Impl {
  int rate = 44100;
  bool ready = false;
  long total_frames = 0;
  std::vector<Ev> events;  // sorted by (frame, type) with off before on
  std::vector<MidiPlayer::Track> chan_rows;
  std::vector<MidiPlayer::Track> instr_rows;

  fluid_settings_t* settings = nullptr;
  fluid_synth_t* synth = nullptr;
  int sfid = -1;

  // Shared with the audio thread (lock-free).
  std::atomic<bool> is_playing{false};
  std::atomic<long> seek_to{-1};  // >=0: pending seek (frames)
  std::atomic<bool> honor{true};
  std::atomic<long> pos_report{0};
  std::atomic<float> chan_gain[16];
  std::atomic<float> instr_gain[kDrumProgram + 1];
  std::atomic<float> chan_lvl[16];
  std::atomic<float> instr_lvl[kDrumProgram + 1];

  // Audio-thread only.
  long pos = 0;
  std::size_t ev_idx = 0;
  int cur_prog[16];

  std::vector<float> scratch;   // (2*kGroups) * kChunk planar
  std::vector<float*> bufptr;

  Impl() {
    for (int i = 0; i < 16; ++i) {
      chan_gain[i] = 1.0f;
      chan_lvl[i] = 0.0f;
      cur_prog[i] = 0;
    }
    for (int i = 0; i <= kDrumProgram; ++i) {
      instr_gain[i] = 1.0f;
      instr_lvl[i] = 0.0f;
    }
  }
};

MidiPlayer::MidiPlayer(const std::string& soundfont_path,
                       const std::vector<std::uint8_t>& midi_bytes,
                       int sample_rate)
    : p_(new Impl) {
  p_->rate = sample_rate > 0 ? sample_rate : 44100;

  // Same audio-driver probe noise as MidiSynth; mute it during setup.
  fluid_log_function_t prev =
      fluid_set_log_function(FLUID_WARN, silent_log, nullptr);
  p_->settings = new_fluid_settings();
  fluid_settings_setnum(p_->settings, "synth.sample-rate",
                        static_cast<double>(p_->rate));
  fluid_settings_setint(p_->settings, "synth.reverb.active", 0);
  fluid_settings_setint(p_->settings, "synth.chorus.active", 0);
  fluid_settings_setnum(p_->settings, "synth.gain", 0.6);
  fluid_settings_setint(p_->settings, "synth.polyphony", 256);
  // Render each MIDI channel into its own stereo group so we can meter and
  // gain them independently (channel c -> group c).
  fluid_settings_setint(p_->settings, "synth.audio-channels", kGroups);
  fluid_settings_setint(p_->settings, "synth.audio-groups", kGroups);
  p_->synth = new_fluid_synth(p_->settings);
  fluid_set_log_function(FLUID_WARN, prev, nullptr);
  if (p_->synth)
    p_->sfid = fluid_synth_sfload(p_->synth, soundfont_path.c_str(), 1);
  if (!p_->synth || p_->sfid < 0)
    throw std::runtime_error("MidiPlayer: could not load SoundFont (FluidSynth)");

  const MidiSong song = parse_smf(midi_bytes);

  // tick -> seconds from the tempo map (same math as the converter).
  std::vector<double> spt(song.tempos.size()), cum(song.tempos.size());
  cum[0] = 0.0;
  spt[0] = song.tempos[0].usec_per_qn / 1.0e6 / song.division;
  for (std::size_t i = 1; i < song.tempos.size(); ++i) {
    spt[i] = song.tempos[i].usec_per_qn / 1.0e6 / song.division;
    cum[i] = cum[i - 1] +
             static_cast<double>(song.tempos[i].tick - song.tempos[i - 1].tick) *
                 spt[i - 1];
  }
  auto sec = [&](long tick) {
    std::size_t i = 0;
    while (i + 1 < song.tempos.size() && song.tempos[i + 1].tick <= tick) ++i;
    return cum[i] + static_cast<double>(tick - song.tempos[i].tick) * spt[i];
  };
  auto frame = [&](long tick) {
    return static_cast<long>(std::llround(sec(tick) * p_->rate));
  };

  long chan_notes[16] = {0};
  int chan_prog[16];
  bool chan_prog_set[16] = {false};
  bool chan_used[16] = {false};
  for (int i = 0; i < 16; ++i) chan_prog[i] = 0;
  std::map<int, long> instr_notes;

  for (const MidiNote& n : song.notes) {
    p_->events.push_back({frame(n.tick_on), 1, n.channel, n.key, n.velocity,
                          n.program, n.volume, n.expression, n.pan, n.drum});
    p_->events.push_back(
        {frame(n.tick_off), 0, n.channel, n.key, 0, n.program, 0, 0, 0, n.drum});
    chan_used[n.channel] = true;
    ++chan_notes[n.channel];
    if (!chan_prog_set[n.channel]) {
      chan_prog[n.channel] = n.program;
      chan_prog_set[n.channel] = true;
    }
    ++instr_notes[instrument_of(n.program, n.drum)];
  }
  std::sort(p_->events.begin(), p_->events.end(), [](const Ev& a, const Ev& b) {
    return a.frame != b.frame ? a.frame < b.frame : a.type < b.type;
  });
  p_->total_frames = frame(song.end_tick) + static_cast<long>(p_->rate) * 2;

  for (int ch = 0; ch < 16; ++ch) {
    if (!chan_used[ch]) continue;
    MidiPlayer::Track t;
    t.channel = ch;
    t.drum = (ch == 9);
    t.program = t.drum ? kDrumProgram : chan_prog[ch];
    t.notes = chan_notes[ch];
    t.name = "Ch " + std::to_string(ch + 1) + " - " + gm_name(t.program);
    p_->chan_rows.push_back(t);
  }
  for (const auto& kv : instr_notes) {
    MidiPlayer::Track t;
    t.channel = -1;
    t.program = kv.first;
    t.drum = (kv.first == kDrumProgram);
    t.notes = kv.second;
    t.name = gm_name(kv.first);
    p_->instr_rows.push_back(t);
  }

  p_->scratch.assign(static_cast<std::size_t>(2 * kGroups * kChunk), 0.0f);
  p_->bufptr.resize(2 * kGroups);
  for (int i = 0; i < 2 * kGroups; ++i)
    p_->bufptr[i] = p_->scratch.data() + static_cast<std::size_t>(i) * kChunk;

  p_->ready = true;
}

MidiPlayer::~MidiPlayer() {
  if (p_->synth) delete_fluid_synth(p_->synth);
  if (p_->settings) delete_fluid_settings(p_->settings);
}

bool MidiPlayer::ok() const { return p_->ready; }
int MidiPlayer::sample_rate() const { return p_->rate; }
const std::vector<MidiPlayer::Track>& MidiPlayer::channels() const {
  return p_->chan_rows;
}
const std::vector<MidiPlayer::Track>& MidiPlayer::instruments() const {
  return p_->instr_rows;
}

void MidiPlayer::play() { p_->is_playing = true; }
void MidiPlayer::stop() { p_->is_playing = false; }
void MidiPlayer::toggle() { p_->is_playing = !p_->is_playing.load(); }
bool MidiPlayer::playing() const { return p_->is_playing.load(); }

void MidiPlayer::seek(double seconds) {
  long f = static_cast<long>(std::llround(seconds * p_->rate));
  if (f < 0) f = 0;
  if (f > p_->total_frames) f = p_->total_frames;
  p_->seek_to = f;
}
double MidiPlayer::position() const {
  return static_cast<double>(p_->pos_report.load()) / p_->rate;
}
double MidiPlayer::duration() const {
  return static_cast<double>(p_->total_frames) / p_->rate;
}

void MidiPlayer::set_channel_gain(int ch, float g) {
  if (ch >= 0 && ch < 16) p_->chan_gain[ch] = g;
}
float MidiPlayer::channel_gain(int ch) const {
  return (ch >= 0 && ch < 16) ? p_->chan_gain[ch].load() : 1.0f;
}
void MidiPlayer::set_instrument_gain(int prog, float g) {
  if (prog >= 0 && prog <= kDrumProgram) p_->instr_gain[prog] = g;
}
float MidiPlayer::instrument_gain(int prog) const {
  return (prog >= 0 && prog <= kDrumProgram) ? p_->instr_gain[prog].load() : 1.0f;
}
void MidiPlayer::set_honor_cc(bool on) { p_->honor = on; }
bool MidiPlayer::honor_cc() const { return p_->honor.load(); }

MidiMix MidiPlayer::snapshot() const {
  MidiMix m;
  for (int i = 0; i < 16; ++i) m.channel_gain[i] = p_->chan_gain[i].load();
  for (const auto& t : p_->instr_rows)
    m.program_gain[t.program] = p_->instr_gain[t.program].load();
  m.honor_cc = p_->honor.load();
  return m;
}

float MidiPlayer::channel_level(int ch) const {
  return (ch >= 0 && ch < 16) ? p_->chan_lvl[ch].load() : 0.0f;
}
float MidiPlayer::instrument_level(int prog) const {
  return (prog >= 0 && prog <= kDrumProgram) ? p_->instr_lvl[prog].load() : 0.0f;
}

void MidiPlayer::render(float* out, int frames) {
  Impl& s = *p_;
  for (int i = 0; i < frames * 2; ++i) out[i] = 0.0f;
  if (!s.ready) return;

  const long sk = s.seek_to.exchange(-1);
  if (sk >= 0) {
    s.pos = sk;
    fluid_synth_all_sounds_off(s.synth, -1);
    s.ev_idx = 0;
    while (s.ev_idx < s.events.size() && s.events[s.ev_idx].frame < s.pos)
      ++s.ev_idx;
  }

  if (!s.is_playing.load()) {
    for (int i = 0; i < 16; ++i) s.chan_lvl[i] = s.chan_lvl[i].load() * kMeterDecay;
    for (int i = 0; i <= kDrumProgram; ++i)
      s.instr_lvl[i] = s.instr_lvl[i].load() * kMeterDecay;
    s.pos_report = s.pos;
    return;
  }

  const bool honor = s.honor.load();
  int done = 0;
  while (done < frames) {
    const int n = std::min(kChunk, frames - done);
    const long block_end = s.pos + n;

    while (s.ev_idx < s.events.size() && s.events[s.ev_idx].frame < block_end) {
      const Ev& e = s.events[s.ev_idx++];
      if (e.type == 1) {
        fluid_synth_cc(s.synth, e.ch, 7, honor ? e.vol : 100);
        fluid_synth_cc(s.synth, e.ch, 11, honor ? e.expr : 127);
        fluid_synth_cc(s.synth, e.ch, 10, honor ? e.pan : 64);
        if (e.drum)
          fluid_synth_program_select(s.synth, e.ch, s.sfid, 128, 0);
        else
          fluid_synth_program_select(s.synth, e.ch, s.sfid, 0, e.program);
        s.cur_prog[e.ch] = e.drum ? kDrumProgram : e.program;
        fluid_synth_noteon(s.synth, e.ch, e.key, e.vel);
      } else {
        fluid_synth_noteoff(s.synth, e.ch, e.key);
      }
    }

    for (int b = 0; b < 2 * kGroups; ++b) std::fill(s.bufptr[b], s.bufptr[b] + n, 0.0f);
    fluid_synth_process(s.synth, n, 0, nullptr, 2 * kGroups, s.bufptr.data());

    float ipeak[kDrumProgram + 1] = {0};
    for (int ch = 0; ch < 16; ++ch) {
      const float* L = s.bufptr[2 * ch];
      const float* R = s.bufptr[2 * ch + 1];
      const int instr = (ch == 9) ? kDrumProgram : s.cur_prog[ch];
      const float g = s.chan_gain[ch].load() * s.instr_gain[instr].load();
      float peak = 0.0f;
      for (int i = 0; i < n; ++i) {
        const float l = L[i] * g, r = R[i] * g;
        out[(done + i) * 2] += l;
        out[(done + i) * 2 + 1] += r;
        const float a = std::max(std::fabs(l), std::fabs(r));
        if (a > peak) peak = a;
      }
      float clvl = std::max(peak, s.chan_lvl[ch].load() * kMeterDecay);
      s.chan_lvl[ch] = clvl > 1.0f ? 1.0f : clvl;
      if (peak > ipeak[instr]) ipeak[instr] = peak;
    }
    for (int i = 0; i <= kDrumProgram; ++i) {
      float il = std::max(ipeak[i], s.instr_lvl[i].load() * kMeterDecay);
      s.instr_lvl[i] = il > 1.0f ? 1.0f : il;
    }

    s.pos = block_end;
    done += n;
    if (s.pos >= s.total_frames) {  // reached the end: stop and rewind
      s.is_playing = false;
      s.pos = 0;
      fluid_synth_all_sounds_off(s.synth, -1);
      s.ev_idx = 0;
      break;
    }
  }
  s.pos_report = s.pos;
}

}  // namespace circus2bmson
