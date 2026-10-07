#pragma once

#include "inference/backend.h"

#include <filesystem>
#include <mutex>

struct llama_context;
struct llama_model;

namespace helix::inference {

// Runs GGUF models with llama.cpp. Models are the *.gguf files in `modelDir`; the first request loads one (the named
// model, else the only or first file). Requests are served one at a time by the queue, so no locking is needed during
// generation; `mutex_` only guards what models() and snapshot() read from other threads.
class LlamaBackend : public Backend {
public:
  struct Config {
    std::filesystem::path modelDir;
    std::int32_t gpuLayers = -1;            // -1 = offload everything the GPU can hold
    std::uint32_t contextTokens = 4096;     // per request, capped by what the model was trained for
  };

  explicit LlamaBackend(Config config);
  ~LlamaBackend() override;

  std::vector<ModelInfo> models() override;
  bool load(const std::string& model, std::string& error) override;
  void unload() override;
  bool isLoaded() const override;
  Outcome chat(const ChatRequest& request, const std::function<void(const std::string&)>& onDelta,
               const std::function<bool()>& cancelled) override;
  nlohmann::json snapshot() const override;

private:
  std::string formatPrompt(const nlohmann::json& messages) const;
  // The context outlives a request so the start of the next prompt (the tool list, the examples) need not be processed
  // again. `kvTokens_` is exactly what its memory holds. Touched only by the worker and by unload(), which the queue never
  // runs while a request is in flight.
  bool ensureContext(std::uint32_t contextTokens);
  void invalidateCache();

  Config config_;
  mutable std::mutex mutex_;
  llama_model* model_ = nullptr;
  llama_context* context_ = nullptr;
  std::uint32_t contextTokens_ = 0;
  std::vector<std::int32_t> kvTokens_;
  std::string loaded_;
};

}  // namespace helix::inference
