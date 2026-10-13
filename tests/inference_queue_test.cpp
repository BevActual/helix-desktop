#include "inference/fake_backend.h"
#include "inference/inference_queue.h"
#include "tests/test_check.h"

#include <future>
#include <map>

using namespace helix::inference;
using nlohmann::json;

namespace {

  json messages(const std::string& text = "hello") { return json::array({{{"role", "user"}, {"content", text}}}); }

  ChatRequest request(const std::string& client, Priority priority = Priority::Interactive) {
    ChatRequest r;
    r.client = client;
    r.messages = messages();
    r.priority = priority;
    return r;
  }

  // Collects what the queue reports and lets a test wait for a particular operation to finish.
  struct Recorder {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint64_t, std::string> deltas;
    std::map<std::uint64_t, Outcome> done;
    std::vector<std::uint64_t> order; // completion order

    InferenceQueue::DeltaFn onDelta() {
      return [this](std::uint64_t op, const std::string& text) {
        std::lock_guard lock(mutex);
        deltas[op] += text;
      };
    }
    InferenceQueue::DoneFn onDone() {
      return [this](std::uint64_t op, const Outcome& outcome) {
        {
          std::lock_guard lock(mutex);
          done[op] = outcome;
          order.push_back(op);
        }
        changed.notify_all();
      };
    }
    bool wait(std::uint64_t op) {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, std::chrono::seconds(5), [&] { return done.contains(op); });
    }
  };

  // Holds the first request on the worker until released, so the queue behind it can be arranged deterministically.
  struct Gate {
    std::promise<void> entered, release;
    std::shared_future<void> releaseFuture = release.get_future().share();
    void hook(FakeBackend& backend) {
      backend.onStart = [this, first = true](const ChatRequest&) mutable {
        if (!first)
          return;
        first = false;
        entered.set_value();
        releaseFuture.wait();
      };
    }
  };

  void checkStreaming() {
    auto backend = std::make_shared<FakeBackend>();
    backend->reply = "hello world";
    Recorder rec;
    InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());
    const auto op = queue.submit(request("a"));
    TEST_CHECK(op.has_value() && rec.wait(*op));
    TEST_CHECK(rec.done[*op].ok && rec.done[*op].text == "hello world");
    TEST_CHECK(rec.deltas[*op] == "hello world"); // the pieces add up to the whole reply
  }

  void checkPriority() {
    auto backend = std::make_shared<FakeBackend>();
    Gate gate;
    gate.hook(*backend);
    Recorder rec;
    InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());
    const auto first = *queue.submit(request("a", Priority::Background)); // starts at once and is held
    gate.entered.get_future().wait();
    const auto bg = *queue.submit(request("a", Priority::Background));
    const auto interactive1 = *queue.submit(request("b", Priority::Interactive));
    const auto interactive2 = *queue.submit(request("b", Priority::Interactive));
    gate.release.set_value();
    TEST_CHECK(rec.wait(bg) && rec.wait(interactive2));
    // The running request is never preempted, then interactive work goes first, in order, then background.
    TEST_CHECK((rec.order == std::vector<std::uint64_t>{first, interactive1, interactive2, bg}));
  }

  void checkLimits() {
    auto backend = std::make_shared<FakeBackend>();
    Gate gate;
    gate.hook(*backend);
    Recorder rec;
    Limits limits;
    limits.maxPending = 4;
    limits.maxPerClient = 2;
    InferenceQueue queue(backend, limits, rec.onDelta(), rec.onDone());

    TEST_CHECK(queue.submit(request("a")).has_value()); // runs, held
    gate.entered.get_future().wait();
    TEST_CHECK(queue.submit(request("a")).has_value());
    auto third = queue.submit(request("a"));
    TEST_CHECK(
        !third.has_value() && third.error().find("too many") != std::string::npos
    ); // per-client cap, running counts
    TEST_CHECK(queue.submit(request("b")).has_value());
    TEST_CHECK(queue.submit(request("c")).has_value());
    auto full = queue.submit(request("d"));
    TEST_CHECK(!full.has_value() && full.error().find("full") != std::string::npos); // total cap

    ChatRequest bad = request("e");
    bad.messages = json::array();
    TEST_CHECK(!queue.submit(bad).has_value());
    bad.messages = json::array({{{"role", "user"}}}); // no content
    TEST_CHECK(!queue.submit(bad).has_value());
    bad.messages = messages(std::string(70 * 1024, 'x'));
    TEST_CHECK(!queue.submit(bad).has_value()); // over the size limit
    gate.release.set_value();
  }

  void checkClamping() {
    auto backend = std::make_shared<FakeBackend>();
    Recorder rec;
    InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());
    ChatRequest r = request("a");
    r.maxTokens = 100000;
    r.temperature = 50.0;
    const auto op = *queue.submit(r);
    TEST_CHECK(rec.wait(op));
    const auto seen = backend->seen();
    TEST_CHECK(seen.size() == 1 && seen[0].maxTokens == Limits{}.maxTokensCap && seen[0].temperature == 2.0);
  }

  void checkCancel() {
    auto backend = std::make_shared<FakeBackend>();
    backend->reply = std::string(2000, 'x');
    backend->chunkChars = 1;
    backend->perChunk = std::chrono::milliseconds(2);
    Gate gate;
    gate.hook(*backend);
    Recorder rec;
    InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());

    const auto running = *queue.submit(request("a"));
    gate.entered.get_future().wait();
    const auto waiting = *queue.submit(request("a"));
    TEST_CHECK(!queue.cancel("someone-else", waiting)); // only the requester may cancel
    TEST_CHECK(queue.cancel("a", waiting));
    TEST_CHECK(rec.wait(waiting) && rec.done[waiting].cancelled && !rec.done[waiting].ok);
    TEST_CHECK(backend->seen().size() == 1); // the cancelled request never reached the backend
    TEST_CHECK(!queue.cancel("a", waiting)); // already gone

    TEST_CHECK(!queue.cancel("someone-else", running));
    TEST_CHECK(queue.cancel("a", running)); // a running request stops at the next token
    gate.release.set_value();
    TEST_CHECK(rec.wait(running) && rec.done[running].cancelled);
    TEST_CHECK(rec.done[running].text.size() < 2000);
  }

  void checkModelLoading() {
    auto backend = std::make_shared<FakeBackend>();
    Recorder rec;
    InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());
    ChatRequest r = request("a");
    r.model = "fake-model";
    TEST_CHECK(rec.wait(*queue.submit(r)) && backend->loadedModel() == "fake-model");
    r.model = "does-not-exist";
    const auto op = *queue.submit(r);
    TEST_CHECK(rec.wait(op) && !rec.done[op].ok && rec.done[op].error.find("no such model") != std::string::npos);
    TEST_CHECK(queue.snapshot().at("queue").at("waiting") == 0 && queue.snapshot().at("backend") == "fake");
  }

  void checkIdleUnload() {
    auto backend = std::make_shared<FakeBackend>();
    Gate gate;
    gate.hook(*backend);
    Recorder rec;
    InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());
    using namespace std::chrono_literals;

    TEST_CHECK(!queue.unloadIfIdle(0s)); // nothing loaded yet: nothing to free
    ChatRequest r = request("a");
    r.model = "fake-model";
    const auto op = *queue.submit(r);
    gate.entered.get_future().wait();
    TEST_CHECK(!queue.unloadIfIdle(0s) && backend->loadedModel() == "fake-model"); // never while a request is running
    const auto next = *queue.submit(request("a"));
    gate.release.set_value();
    TEST_CHECK(rec.wait(op) && rec.wait(next));
    TEST_CHECK(!queue.unloadIfIdle(1h) && backend->isLoaded()); // recently used: kept
    TEST_CHECK(queue.unloadIfIdle(0s) && !backend->isLoaded()); // idle long enough: freed
    TEST_CHECK(!queue.unloadIfIdle(0s));                        // and only once

    r.model = "fake-model"; // the next request loads it again
    TEST_CHECK(rec.wait(*queue.submit(r)) && backend->isLoaded());
  }

  void checkShutdown() {
    auto backend = std::make_shared<FakeBackend>();
    Gate gate;
    gate.hook(*backend);
    Recorder rec;
    std::uint64_t running = 0, waiting = 0;
    {
      InferenceQueue queue(backend, Limits{}, rec.onDelta(), rec.onDone());
      running = *queue.submit(request("a"));
      gate.entered.get_future().wait();
      waiting = *queue.submit(request("b"));
      std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gate.release.set_value();
      });
      release.detach();
    } // destruction must not hang, and must finish everything it accepted
    TEST_CHECK(rec.done.contains(running) && rec.done.contains(waiting));
    TEST_CHECK(!rec.done[waiting].ok && rec.done[waiting].error.find("shutting down") != std::string::npos);
  }

} // namespace

int main() {
  checkStreaming();
  checkPriority();
  checkLimits();
  checkClamping();
  checkCancel();
  checkIdleUnload();
  checkModelLoading();
  checkShutdown();
  return 0;
}
