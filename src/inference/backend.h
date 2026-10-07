#pragma once

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The seam between the inference service and an engine. The service owns scheduling, limits and D-Bus; a backend only
// turns one request into text. Backends run on the service's single worker thread and are never called concurrently.
namespace helix::inference {

enum class Priority { Interactive, Background };

struct ChatRequest {
  std::string client;                  // verified identity of the caller, for limits and logging
  std::string model;                   // empty = the backend's default
  nlohmann::json messages;             // [{role, content}, ...]
  std::string jsonSchema;              // when set the output must conform to it (structured output); empty = free text
  std::uint32_t maxTokens = 256;
  double temperature = 0.0;
  Priority priority = Priority::Interactive;
};

struct Outcome {
  bool ok = false;
  std::string text;                    // everything produced, including after a cancellation
  std::string error;                   // set when !ok
  bool cancelled = false;
  bool truncated = false;              // stopped by the token limit before the model finished
  std::uint32_t promptTokens = 0;
  std::uint32_t completionTokens = 0;
};

struct ModelInfo {
  std::string id;                      // file name without directory
  std::uint64_t bytes = 0;
  bool loaded = false;
};

class Backend {
public:
  virtual ~Backend() = default;

  virtual std::vector<ModelInfo> models() = 0;
  // Called before a request that names a model that is not the current one.
  virtual bool load(const std::string& model, std::string& error) = 0;
  virtual void unload() = 0;

  // Produces the reply, calling `onDelta` with each new piece. Must return promptly once `cancelled()` is true.
  virtual Outcome chat(const ChatRequest& request, const std::function<void(const std::string&)>& onDelta,
                       const std::function<bool()>& cancelled) = 0;

  virtual nlohmann::json snapshot() const = 0;
};

}  // namespace helix::inference
