// A C ABI over helix::inference::LlamaBackend, so the Rust service can drive the llama.cpp engine. The engine code
// (src/inference/llama_backend.cpp) is llama.cpp glue and stays C++; everything around it (D-Bus, queue, limits, routing,
// workers) is Rust. Data crosses as JSON strings; the caller frees returned strings with hx_free.
#include "inference/llama_backend.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>

namespace {

  using helix::inference::ChatRequest;
  using helix::inference::ImageInput;
  using helix::inference::LlamaBackend;
  using nlohmann::json;

  char* copy(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
  }

  LlamaBackend& self(void* handle) { return *static_cast<LlamaBackend*>(handle); }

} // namespace

extern "C" {

typedef void (*hx_delta_fn)(void* user, const char* text);
typedef int (*hx_cancel_fn)(void* user);
typedef void (*hx_progress_fn)(void* user, const char* model, double fraction);

void hx_free(char* text) { std::free(text); }

void* hx_llama_create(const char* model_dir) {
  LlamaBackend::Config config;
  config.modelDir = model_dir;
  return new LlamaBackend(std::move(config));
}

void hx_llama_destroy(void* handle) { delete static_cast<LlamaBackend*>(handle); }

char* hx_llama_models(void* handle) {
  json out = json::array();
  for (const auto& model : self(handle).models())
    out.push_back({{"id", model.id}, {"bytes", model.bytes}, {"loaded", model.loaded}, {"vision", model.vision}});
  return copy(out.dump());
}

// Returns 1 on success; on failure 0 and *error is a string to free.
int hx_llama_load(void* handle, const char* model, char** error) {
  std::string message;
  const bool ok = self(handle).load(model, message);
  if (!ok && error)
    *error = copy(message);
  return ok ? 1 : 0;
}

void hx_llama_unload(void* handle) { self(handle).unload(); }
int hx_llama_is_loaded(void* handle) { return self(handle).isLoaded() ? 1 : 0; }
char* hx_llama_snapshot(void* handle) { return copy(self(handle).snapshot().dump()); }

void hx_llama_set_progress(void* handle, hx_progress_fn callback, void* user) {
  self(handle).setProgress([callback, user](const std::string& model, double fraction) {
    if (callback)
      callback(user, model.c_str(), fraction);
  });
}

// `request` is {model, messages, schema, reason_first, max_tokens, temperature}; pictures are 8-bit RGB. Returns the
// outcome as JSON.
char* hx_llama_chat(
    void* handle, const char* request, const uint8_t* const* pictures, const uint32_t* widths, const uint32_t* heights,
    size_t picture_count, hx_delta_fn on_delta, hx_cancel_fn cancelled, void* user
) {
  const json in = json::parse(request, nullptr, false);
  ChatRequest chat;
  if (in.is_object()) {
    chat.model = in.value("model", std::string());
    chat.messages = in.value("messages", json::array());
    chat.jsonSchema = in.value("schema", std::string());
    chat.reasonFirst = in.value("reason_first", false);
    chat.maxTokens = in.value("max_tokens", 256U);
    chat.temperature = in.value("temperature", 0.0);
  }
  for (size_t i = 0; i < picture_count; ++i) {
    ImageInput image;
    image.width = widths[i];
    image.height = heights[i];
    image.rgb.assign(pictures[i], pictures[i] + static_cast<size_t>(widths[i]) * heights[i] * 3);
    chat.images.push_back(std::move(image));
  }
  const auto outcome = self(handle).chat(
      chat,
      [&](const std::string& piece) {
        if (on_delta)
          on_delta(user, piece.c_str());
      },
      [&] { return cancelled && cancelled(user) != 0; }
  );
  return copy(json{
      {"ok", outcome.ok},
      {"text", outcome.text},
      {"error", outcome.error},
      {"cancelled", outcome.cancelled},
      {"truncated", outcome.truncated},
      {"prompt_tokens", outcome.promptTokens},
      {"cached_prompt_tokens", outcome.cachedPromptTokens},
      {"completion_tokens", outcome.completionTokens},
  }.dump());
}

} // extern "C"
