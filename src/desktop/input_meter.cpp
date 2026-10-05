#include "desktop/input_meter.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/pod/builder.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace helix {
double inputPeak(std::span<const std::byte> bytes) {
  double peak = 0;
  for (std::size_t offset = 0; offset + sizeof(float) <= bytes.size(); offset += sizeof(float)) {
    float sample; std::memcpy(&sample, bytes.data() + offset, sizeof(sample));
    if (std::isfinite(sample)) peak = std::max(peak, std::abs(double(sample)));
  }
  return std::clamp(peak, 0.0, 1.0);
}
struct InputMeter::Impl {
  pw_stream* stream = nullptr;
  spa_hook listener{};
  double peak = 0;
  bool streaming = false;
  ~Impl() { if (stream) { spa_hook_remove(&listener); pw_stream_destroy(stream); } }
};
InputMeter::InputMeter(pw_core* core, std::uint32_t source) : impl(std::make_unique<Impl>()) {
  auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Production", PW_KEY_APP_NAME, "Helix Settings microphone test", nullptr);
  impl->stream = pw_stream_new(core, "Helix microphone level", properties);
  if (!impl->stream) throw std::runtime_error("Microphone test could not start");
  static const pw_stream_events events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = +[](void* data, pw_stream_state, pw_stream_state state, const char*) {
      auto& self = *static_cast<Impl*>(data); self.streaming = state == PW_STREAM_STATE_STREAMING;
      if (!self.streaming) self.peak = 0;
    },
    .process = +[](void* data) {
      auto& self = *static_cast<Impl*>(data);
      auto* buffer = pw_stream_dequeue_buffer(self.stream); if (!buffer) return;
      self.peak = 0;
      for (std::uint32_t i = 0; i < buffer->buffer->n_datas; ++i) {
        const auto& block = buffer->buffer->datas[i];
        if (!block.data || !block.chunk || block.chunk->offset > block.maxsize) continue;
        const auto length = std::min(block.chunk->size, block.maxsize - block.chunk->offset);
        auto* start = static_cast<const std::byte*>(block.data) + block.chunk->offset;
        self.peak = std::max(self.peak, inputPeak({start, length}));
      }
      pw_stream_queue_buffer(self.stream, buffer);
    }
  };
  // No RT_PROCESS flag: callbacks run on the service's dispatched PipeWire loop.
  pw_stream_add_listener(impl->stream, &impl->listener, &events, impl.get());
  std::uint8_t storage[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_F32; format.rate = 48000; format.channels = 1;
  format.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
  const auto flags = pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT);
  if (pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, source, flags, params, 1) < 0)
    throw std::runtime_error("Microphone test could not connect");
}
InputMeter::~InputMeter() = default;
double InputMeter::peak() const { return impl->peak; }
bool InputMeter::available() const { return impl->streaming; }
}
