// miniaudio device wrapper. This is the single translation unit that compiles
// the miniaudio implementation; everything the GUI needs is exposed through the
// opaque interface in audio.hpp.
#include "audio.hpp"

#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio/miniaudio.h"

#include "circus2bmson/midiplayer.hpp"

struct AudioDevice {
  ma_device dev;
};

namespace {
void data_cb(ma_device* d, void* output, const void* /*input*/,
             ma_uint32 frames) {
  auto* player = static_cast<circus2bmson::MidiPlayer*>(d->pUserData);
  if (player)
    player->render(static_cast<float*>(output), static_cast<int>(frames));
  else
    std::memset(output, 0, static_cast<std::size_t>(frames) * 2 * sizeof(float));
}
}  // namespace

AudioDevice* audio_open(circus2bmson::MidiPlayer* player) {
  if (!player) return nullptr;
  auto* a = new AudioDevice();
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = static_cast<ma_uint32>(player->sample_rate());
  cfg.dataCallback = data_cb;
  cfg.pUserData = player;
  if (ma_device_init(nullptr, &cfg, &a->dev) != MA_SUCCESS) {
    delete a;
    return nullptr;
  }
  if (ma_device_start(&a->dev) != MA_SUCCESS) {
    ma_device_uninit(&a->dev);
    delete a;
    return nullptr;
  }
  return a;
}

void audio_close(AudioDevice* a) {
  if (!a) return;
  ma_device_uninit(&a->dev);
  delete a;
}
