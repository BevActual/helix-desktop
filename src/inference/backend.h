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

  // One decoded picture handed to a vision model: 8-bit RGB, row after row. Produced by decodeImageInput
  // (image_input.h).
  struct ImageInput {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb; // width * height * 3
  };

  struct ChatRequest {
    std::string client;             // verified identity of the caller, for limits and logging
    std::string model;              // empty = the backend's default
    nlohmann::json messages;        // [{role, content}, ...]; content is a string or a list of parts:
                                    //   {"type":"text","text":...} | {"type":"image","index":N} (an entry of `images`)
    std::vector<ImageInput> images; // empty for a text-only request
    bool generateImage = false;
    std::string imagePrompt;
    std::uint32_t imageWidth = 512, imageHeight = 512;
    std::uint32_t imageSteps = 4;
    std::uint64_t imageSeed = 0;
    bool transcribe = false;         // a speech-to-text request: `audio` and `language` instead of messages
    std::vector<std::int16_t> audio; // 16 kHz mono samples
    std::string language = "auto";
    std::string jsonSchema;   // when set the output must conform to it (structured output); empty = free text
    bool reasonFirst = false; // with a schema: emit the reason before the tool (see GrammarOptions)
    std::uint32_t maxTokens = 256;
    double temperature = 0.0;
    Priority priority = Priority::Interactive;
  };

  struct Outcome {
    bool ok = false;
    std::string text;  // everything produced, including after a cancellation
    std::string error; // set when !ok
    bool cancelled = false;
    bool truncated = false; // stopped by the token limit before the model finished
    std::uint32_t promptTokens = 0;
    std::uint32_t cachedPromptTokens =
        0; // of those, how many were already in the model's memory from the previous request
    std::uint32_t completionTokens = 0;
    std::string language; // transcription: the language that was spoken
    std::string imagePng; // bounded base64 PNG; no worker-chosen file path crosses the bus
    std::string imageModel;
    std::string imagePrompt;
    std::uint32_t imageWidth = 0, imageHeight = 0;
    std::uint64_t imageSeed = 0;
  };

  struct ModelInfo {
    std::string id; // file name without directory
    std::uint64_t bytes = 0;
    bool loaded = false;
    bool vision = false; // a projector for this model is installed: it can read images
  };

  // A message's content as one string for engines that take text: parts are joined, and each image part becomes
  // `marker`. Returns the images the content refers to, in order, through `used`.
  inline std::string
  flattenContent(const nlohmann::json& content, const std::string& marker, std::vector<std::size_t>* used = nullptr) {
    if (content.is_string())
      return content.get<std::string>();
    std::string out;
    for (const auto& part : content) {
      const auto type = part.value("type", std::string());
      if (type == "text")
        out += part.value("text", std::string());
      else if (type == "image") {
        out += marker;
        if (used)
          used->push_back(part.value("index", std::size_t{0}));
      }
    }
    return out;
  }

  class Backend {
  public:
    virtual ~Backend() = default;

    virtual std::vector<ModelInfo> models() = 0;
    // Called before a request that names a model that is not the current one.
    virtual bool load(const std::string& model, std::string& error) = 0;
    virtual void unload() = 0;
    virtual bool isLoaded() const = 0;

    // Produces the reply, calling `onDelta` with each new piece. Must return promptly once `cancelled()` is true.
    virtual Outcome chat(
        const ChatRequest& request, const std::function<void(const std::string&)>& onDelta,
        const std::function<bool()>& cancelled
    ) = 0;

    virtual nlohmann::json snapshot() const = 0;

    // Told how far loading a model has got (0..1) while load() runs, on the worker thread. Optional.
    using ProgressFn = std::function<void(const std::string& model, double fraction)>;
    virtual void setProgress(ProgressFn) {}
  };

} // namespace helix::inference
