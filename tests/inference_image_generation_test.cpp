#include "inference/image_generator.h"
#include "inference/image_input.h"
#include "inference/inference_queue.h"
#include "inference/fake_backend.h"
#include <cassert>
#include <condition_variable>
#include <glib.h>
#include <iostream>

using namespace helix::inference;
int main() {
  ChatRequest request;
  request.generateImage = true; request.imagePrompt = "a blue mug"; request.client = "test";
  assert(validateImageRequest(request));
  auto bad = request; bad.imageWidth = 999999; assert(!validateImageRequest(bad));
  bad = request; bad.imageHeight = 511; assert(!validateImageRequest(bad));
  bad = request; bad.imageSteps = 100; assert(!validateImageRequest(bad));
  bad = request; bad.imagePrompt = std::string(4001, 'x'); assert(!validateImageRequest(bad));
  bad = request; bad.model = "../../private.gguf"; assert(!validateImageRequest(bad));
  bad = request; bad.imagePrompt = std::string("hi\0there", 8); assert(!validateImageRequest(bad));
  bad = request; bad.imageSeed = 2147483648ULL; assert(!validateImageRequest(bad));
  assert(encodePng({}, 512, 512).empty());

  auto backend = std::make_shared<FakeBackend>();
  std::string error; assert(backend->load("fake-model", error));
  std::mutex mutex; std::condition_variable wake; std::vector<Outcome> results;
  InferenceQueue queue(backend, {}, [](auto, auto&) {}, [&](auto, const Outcome& outcome) { std::lock_guard lock(mutex); results.push_back(outcome); wake.notify_all(); });
  queue.setImageGenerator(std::make_shared<FakeImageGenerator>());
  auto operation = queue.submit(request); assert(operation);
  {
    std::unique_lock lock(mutex); assert(wake.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == 1; }));
  }
  assert(results[0].ok && !results[0].imagePng.empty());
  assert(!backend->isLoaded());  // the chat model gave up GPU residency before diffusion ran
  gsize size = 0; guchar* png = g_base64_decode(results[0].imagePng.c_str(), &size);
  auto image = decodeImageInput(png, size); g_free(png);
  assert(image && image->width == 512 && image->height == 512);
  operation = queue.submit(request); assert(operation);
  assert(!queue.cancel("another-client", *operation)); assert(queue.cancel("test", *operation));
  {
    std::unique_lock lock(mutex); assert(wake.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == 2; }));
  }
  assert(results[1].cancelled);
  std::cout << "image validation, PNG, GPU handoff, and cancellation passed\n";
}
