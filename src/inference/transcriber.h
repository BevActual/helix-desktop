#pragma once

#include "inference/backend.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Speech to text. The engine (whisper.cpp) bundles its own copy of ggml, which cannot share a process with llama.cpp's, and a
// crash while decoding untrusted audio must not take the chat service down, so it runs as a separate, hardened process.
namespace helix::inference {

inline constexpr std::size_t kSampleRate = 16000;
inline constexpr std::size_t kMaxAudioSeconds = 600;
inline constexpr std::size_t kMaxAudioSamples = kSampleRate * kMaxAudioSeconds;
inline constexpr std::size_t kMaxAudioBytes = kMaxAudioSamples * 2;  // signed 16-bit little-endian, mono

// What a client sends: raw samples, 16 kHz mono. No container or codec is parsed anywhere on this path.
struct AudioInput {
  std::vector<std::int16_t> samples;
};

class Transcriber {
public:
  virtual ~Transcriber() = default;
  // `language` is a two-letter code or "auto". Must return promptly once `cancelled()` is true.
  virtual Outcome transcribe(const AudioInput& audio, const std::string& language, const std::function<bool()>& cancelled) = 0;
};

// Runs the `helix-transcribe` worker per request: samples in on its stdin, one JSON line out.
class ProcessTranscriber final : public Transcriber {
public:
  // `speechDir` holds ggml-*.bin models; the one named by HELIX_SPEECH_MODEL, else the first by name, is used.
  ProcessTranscriber(std::filesystem::path worker, std::filesystem::path speechDir);
  Outcome transcribe(const AudioInput& audio, const std::string& language, const std::function<bool()>& cancelled) override;

private:
  std::filesystem::path worker_;
  std::filesystem::path speechDir_;
};

// Scripted, for tests of the D-Bus layer: reports how much audio arrived.
class FakeTranscriber final : public Transcriber {
public:
  Outcome transcribe(const AudioInput& audio, const std::string& language, const std::function<bool()>&) override {
    Outcome outcome;
    outcome.ok = true;
    outcome.language = language == "auto" ? "en" : language;
    outcome.text = "heard " + std::to_string(audio.samples.size()) + " samples";
    return outcome;
  }
};

}  // namespace helix::inference
