#pragma once

#include "inference/backend.h"

#include <chrono>
#include <mutex>
#include <thread>

namespace helix::inference {

// A scripted backend for tests and for exercising the service without a model: replies with `reply` in small pieces.
class FakeBackend : public Backend {
public:
  std::string reply = "ok";
  std::size_t chunkChars = 3;
  std::chrono::milliseconds perChunk{0};
  std::function<void(const ChatRequest&)> onStart;  // runs first, on the worker; a test can block here to hold a request

  std::vector<ChatRequest> seen() const { std::lock_guard lock(mutex_); return seen_; }
  std::string loadedModel() const { std::lock_guard lock(mutex_); return loaded_; }

  std::vector<ModelInfo> models() override { return {{"fake-model", 1, loadedModel() == "fake-model"}}; }
  bool load(const std::string& model, std::string& error) override {
    if (model != "fake-model") { error = "no such model: " + model; return false; }
    std::lock_guard lock(mutex_);
    loaded_ = model;
    return true;
  }
  void unload() override { std::lock_guard lock(mutex_); loaded_.clear(); }

  Outcome chat(const ChatRequest& request, const std::function<void(const std::string&)>& onDelta,
               const std::function<bool()>& cancelled) override {
    { std::lock_guard lock(mutex_); seen_.push_back(request); }
    if (onStart) onStart(request);
    Outcome outcome;
    for (std::size_t at = 0; at < reply.size(); at += chunkChars) {
      if (cancelled()) { outcome.cancelled = true; outcome.error = "cancelled"; return outcome; }
      if (perChunk.count() > 0) std::this_thread::sleep_for(perChunk);
      const auto piece = reply.substr(at, chunkChars);
      outcome.text += piece;
      onDelta(piece);
    }
    outcome.ok = true;
    outcome.completionTokens = static_cast<std::uint32_t>((reply.size() + chunkChars - 1) / chunkChars);
    return outcome;
  }

  nlohmann::json snapshot() const override { return {{"backend", "fake"}, {"loaded", loadedModel()}}; }

private:
  mutable std::mutex mutex_;
  std::vector<ChatRequest> seen_;
  std::string loaded_;
};

}  // namespace helix::inference
