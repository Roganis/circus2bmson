#ifndef CIRCUS2BMSON_GUI_AUDIO_HPP
#define CIRCUS2BMSON_GUI_AUDIO_HPP

// Tiny opaque wrapper around a miniaudio playback device, so the (huge)
// miniaudio header only compiles in audio.cpp and never in main.cpp. The device
// pulls stereo float frames from the given MidiPlayer on its own audio thread.
namespace circus2bmson {
class MidiPlayer;
}

struct AudioDevice;  // opaque

// Open + start a playback device feeding from `player`. Returns nullptr on
// failure (no audio device, unsupported format, ...). The player must outlive
// the device; call audio_close() before destroying the player.
AudioDevice* audio_open(circus2bmson::MidiPlayer* player);
void audio_close(AudioDevice* dev);

#endif  // CIRCUS2BMSON_GUI_AUDIO_HPP
