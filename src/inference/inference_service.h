#pragma once

#include "inference/inference_queue.h"

#include <chrono>
#include <gio/gio.h>
#include <map>
#include <memory>
#include <string>

namespace helix::inference {

// The D-Bus face of the queue (org.helix.Inference1). The worker thread never touches D-Bus: everything it produces is
// handed to the main loop, where the operation-to-caller table lives. Replies go only to whoever asked.
class InferenceService {
public:
  struct Config {
    std::string modelDir;
    std::chrono::seconds idleUnload{300};
    Limits limits;
  };

  InferenceService(GDBusConnection* bus, Config config, std::shared_ptr<Backend> backend);
  ~InferenceService();

  void method(const std::string& sender, const std::string& name, GVariant* parameters, GDBusMethodInvocation* invocation);
  void ownerGone(const std::string& name);  // cancels what that caller still has queued or running
  void tick();                              // frees the model after a long idle spell
  void setTranscriber(std::shared_ptr<Transcriber> transcriber) { queue_->setTranscriber(std::move(transcriber)); }
  void setImageGenerator(std::shared_ptr<ImageGenerator> generator) { queue_->setImageGenerator(std::move(generator)); }

private:
  struct Owner {
    std::string sender;
    std::string client;
  };

  void post(std::function<void()> task);
  const std::string& clientOf(const std::string& sender);
  void emit(const char* signal, GVariant* value, const std::string& destination);

  GDBusConnection* bus_;
  Config config_;
  std::shared_ptr<Backend> backend_;
  std::map<std::uint64_t, Owner> owners_;      // main thread only
  std::map<std::string, std::string> clients_; // sender -> client identity
  std::unique_ptr<InferenceQueue> queue_;      // last: destroyed first, which stops the worker before the rest goes
};

}  // namespace helix::inference
