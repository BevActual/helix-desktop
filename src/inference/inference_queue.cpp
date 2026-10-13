#include "inference/inference_queue.h"

#include <algorithm>

namespace helix::inference {

InferenceQueue::InferenceQueue(std::shared_ptr<Backend> backend, Limits limits, DeltaFn onDelta, DoneFn onDone)
    : backend_(std::move(backend)), limits_(limits), onDelta_(std::move(onDelta)), onDone_(std::move(onDone)) {
  backend_->setProgress([this](const std::string& model, double fraction) {
    std::uint64_t operation = 0;
    { std::lock_guard lock(mutex_); operation = runningOperation_; }
    if (operation != 0 && onLoading_) onLoading_(operation, model, fraction);
  });
  worker_ = std::thread([this] { run(); });
}

InferenceQueue::~InferenceQueue() {
  std::deque<Entry> abandoned;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    runningCancelled_ = true;
    abandoned.swap(waiting_);
  }
  wake_.notify_all();
  worker_.join();
  for (auto& entry : abandoned) {
    Outcome outcome;
    outcome.error = "the inference service is shutting down";
    onDone_(entry.operation, outcome);
  }
}

std::expected<std::uint64_t, std::string> InferenceQueue::submit(ChatRequest request) {
  if (request.generateImage) {
    if (!imageGenerator_) return std::unexpected("image generation is not available");
    if (auto valid = validateImageRequest(request); !valid) return std::unexpected(valid.error());
    request.messages = nlohmann::json::array();
  } else if (request.transcribe) {
    if (!transcriber_) return std::unexpected("speech to text is not available");
    if (request.audio.empty() || request.audio.size() > kMaxAudioSamples) return std::unexpected("the recording is empty or too long");
    request.messages = nlohmann::json::array();
  } else if (!request.messages.is_array() || request.messages.empty()) {
    return std::unexpected("messages must be a non-empty array");
  }
  for (const auto& message : request.messages) {
    bool wellFormed = message.is_object() && message.contains("role") && message.at("role").is_string() &&
                      message.contains("content");
    if (wellFormed) {
      const auto& content = message.at("content");
      if (content.is_array()) {
        for (const auto& part : content) {
          const bool text = part.is_object() && part.value("type", "") == "text" && part.contains("text") && part.at("text").is_string();
          const bool image = part.is_object() && part.value("type", "") == "image" && part.contains("index") && part.at("index").is_number_unsigned();
          if (!text && !image) wellFormed = false;
        }
      } else if (!content.is_string()) {
        wellFormed = false;
      }
    }
    if (!wellFormed) return std::unexpected("each message needs a string role and a string content (or a list of text and image parts)");
  }
  if (request.messages.dump().size() + request.jsonSchema.size() > limits_.maxPromptBytes) return std::unexpected("the request is too large");
  request.maxTokens = std::clamp<std::uint32_t>(request.maxTokens, 1, limits_.maxTokensCap);
  request.temperature = std::clamp(request.temperature, 0.0, 2.0);

  std::uint64_t operation = 0;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return std::unexpected("the inference service is shutting down");
    const auto running = runningOperation_ != 0 ? 1U : 0U;
    if (waiting_.size() + running >= limits_.maxPending) return std::unexpected("the inference queue is full");
    const auto fromClient = static_cast<std::size_t>(std::ranges::count(waiting_, request.client, [](const Entry& e) -> const std::string& { return e.request.client; })) +
                            (runningOperation_ != 0 && runningClient_ == request.client ? 1 : 0);
    if (fromClient >= limits_.maxPerClient) return std::unexpected("too many requests from this client");
    operation = ++next_;
    lastActivity_ = std::chrono::steady_clock::now();
    // Interactive requests go ahead of waiting background ones, behind earlier interactive ones.
    Entry entry{operation, std::move(request)};
    if (entry.request.priority == Priority::Interactive) {
      const auto firstBackground = std::ranges::find_if(waiting_, [](const Entry& e) { return e.request.priority == Priority::Background; });
      waiting_.insert(firstBackground, std::move(entry));
    } else {
      waiting_.push_back(std::move(entry));
    }
  }
  wake_.notify_one();
  return operation;
}

bool InferenceQueue::cancel(const std::string& client, std::uint64_t operation) {
  {
    std::lock_guard lock(mutex_);
    if (runningOperation_ == operation && operation != 0) {
      if (runningClient_ != client) return false;
      runningCancelled_ = true;  // the worker reports it once the backend stops
      return true;
    }
    const auto found = std::ranges::find(waiting_, operation, &Entry::operation);
    if (found == waiting_.end() || found->request.client != client) return false;
    waiting_.erase(found);
  }
  Outcome outcome;
  outcome.cancelled = true;
  outcome.error = "cancelled";
  onDone_(operation, outcome);
  return true;
}

bool InferenceQueue::unloadIfIdle(std::chrono::steady_clock::duration idle) {
  std::lock_guard lock(mutex_);  // held throughout: the worker cannot start a request while the model is being freed
  if (stopping_ || runningOperation_ != 0 || !waiting_.empty() || !backend_->isLoaded()) return false;
  if (std::chrono::steady_clock::now() - lastActivity_ < idle) return false;
  backend_->unload();
  return true;
}

nlohmann::json InferenceQueue::snapshot() const {
  nlohmann::json out = backend_->snapshot();
  std::lock_guard lock(mutex_);
  out["queue"] = {{"waiting", waiting_.size()}, {"running", runningOperation_ != 0}, {"max", limits_.maxPending}};
  return out;
}

void InferenceQueue::run() {
  for (;;) {
    Entry entry;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !waiting_.empty(); });
      if (stopping_) return;
      entry = std::move(waiting_.front());
      waiting_.pop_front();
      runningOperation_ = entry.operation;
      runningClient_ = entry.request.client;
      runningCancelled_ = false;
    }

    Outcome outcome;
    std::string error;
    if (entry.request.generateImage) {
      if (onProgress_) onProgress_(entry.operation, 0, "Releasing chat model memory");
      backend_->unload();  // shared queue: no chat call can race with this GPU handoff
      outcome = imageGenerator_->generate(entry.request,
        [&](double fraction, const std::string& stage) { if (onProgress_) onProgress_(entry.operation, fraction, stage); },
        [this] { std::lock_guard lock(mutex_); return runningCancelled_; });
    } else if (entry.request.transcribe) {
      outcome = transcriber_->transcribe(AudioInput{std::move(entry.request.audio)}, entry.request.language,
                                         [this] { std::lock_guard lock(mutex_); return runningCancelled_; });
    } else if (!entry.request.model.empty() && !backend_->load(entry.request.model, error)) {
      outcome.error = error.empty() ? "the model could not be loaded" : error;
    } else {
      outcome = backend_->chat(
          entry.request, [&](const std::string& piece) { onDelta_(entry.operation, piece); },
          [this] { std::lock_guard lock(mutex_); return runningCancelled_; });
    }
    bool wasCancelled = false;
    {
      std::lock_guard lock(mutex_);
      wasCancelled = runningCancelled_;
      runningOperation_ = 0;
      runningClient_.clear();
      runningCancelled_ = false;
      lastActivity_ = std::chrono::steady_clock::now();
    }
    if (wasCancelled && !outcome.ok) { outcome.cancelled = true; if (outcome.error.empty()) outcome.error = "cancelled"; }
    onDone_(entry.operation, outcome);
  }
}

}  // namespace helix::inference
