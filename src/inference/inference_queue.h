#pragma once

#include "inference/backend.h"
#include "inference/transcriber.h"
#include "inference/image_generator.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace helix::inference {

struct Limits {
  std::size_t maxPending = 8;               // waiting plus running, across all clients
  std::size_t maxPerClient = 3;
  std::size_t maxPromptBytes = 64 * 1024;   // total size of the messages and the schema
  std::uint32_t maxTokensCap = 1024;        // a request asking for more is clamped
};

// One worker thread runs one request at a time (a model cannot be shared by concurrent generations), choosing
// interactive work before background work and otherwise first come, first served. Callbacks run on the worker thread:
// the D-Bus layer hands them to the main loop.
class InferenceQueue {
public:
  using DeltaFn = std::function<void(std::uint64_t operation, const std::string& text)>;
  using DoneFn = std::function<void(std::uint64_t operation, const Outcome& outcome)>;
  using LoadingFn = std::function<void(std::uint64_t operation, const std::string& model, double fraction)>;
  using ProgressFn = std::function<void(std::uint64_t operation, double fraction, const std::string& stage)>;

  InferenceQueue(std::shared_ptr<Backend> backend, Limits limits, DeltaFn onDelta, DoneFn onDone);
  ~InferenceQueue();  // cancels what is running, finishes everything waiting with "shutting down", joins the worker

  // Refused immediately (nothing queued, no callback) when a limit is exceeded or the request is malformed.
  std::expected<std::uint64_t, std::string> submit(ChatRequest request);
  // Only the requester may cancel. A waiting request completes at once as cancelled; a running one stops at the next
  // token. Returns false for an unknown operation or someone else's.
  bool cancel(const std::string& client, std::uint64_t operation);

  // Frees the model when nothing has run, or been asked to run, for `idle`. Never while a request is running or waiting.
  // Returns true if it unloaded one. The next request loads it again (the first one after is slower).
  bool unloadIfIdle(std::chrono::steady_clock::duration idle);

  // Called on the worker while the running request's model loads. Set before the first request.
  void onLoading(LoadingFn fn) { onLoading_ = std::move(fn); }
  // The engine for speech-to-text requests, if there is one. Set before the first request.
  void setTranscriber(std::shared_ptr<Transcriber> transcriber) { transcriber_ = std::move(transcriber); }
  void setImageGenerator(std::shared_ptr<ImageGenerator> generator) { imageGenerator_ = std::move(generator); }
  void onProgress(ProgressFn fn) { onProgress_ = std::move(fn); }

  nlohmann::json snapshot() const;  // queue depth and the backend's own state

private:
  struct Entry {
    std::uint64_t operation;
    ChatRequest request;
  };

  void run();

  std::shared_ptr<Backend> backend_;
  Limits limits_;
  DeltaFn onDelta_;
  DoneFn onDone_;
  LoadingFn onLoading_;
  std::shared_ptr<Transcriber> transcriber_;
  std::shared_ptr<ImageGenerator> imageGenerator_;
  ProgressFn onProgress_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Entry> waiting_;
  std::uint64_t next_ = 0;
  std::uint64_t runningOperation_ = 0;
  std::string runningClient_;
  bool runningCancelled_ = false;
  bool stopping_ = false;
  std::chrono::steady_clock::time_point lastActivity_ = std::chrono::steady_clock::now();
  std::thread worker_;
};

}  // namespace helix::inference
