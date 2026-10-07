#include "inference/inference_queue.h"

#include <algorithm>

namespace helix::inference {

InferenceQueue::InferenceQueue(std::shared_ptr<Backend> backend, Limits limits, DeltaFn onDelta, DoneFn onDone)
    : backend_(std::move(backend)), limits_(limits), onDelta_(std::move(onDelta)), onDone_(std::move(onDone)) {
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
  if (!request.messages.is_array() || request.messages.empty()) return std::unexpected("messages must be a non-empty array");
  for (const auto& message : request.messages) {
    const bool wellFormed = message.is_object() && message.contains("role") && message.at("role").is_string() &&
                            message.contains("content") && message.at("content").is_string();
    if (!wellFormed) return std::unexpected("each message needs a string role and a string content");
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
    if (!entry.request.model.empty() && !backend_->load(entry.request.model, error)) {
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
    }
    if (wasCancelled && !outcome.ok) { outcome.cancelled = true; if (outcome.error.empty()) outcome.error = "cancelled"; }
    onDone_(entry.operation, outcome);
  }
}

}  // namespace helix::inference
