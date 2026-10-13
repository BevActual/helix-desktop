#include "inference/image_generator.h"
#include "inference/image_input.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <glib.h>
#include <png.h>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace helix::inference {
namespace fs = std::filesystem;

std::expected<void, std::string> validateImageRequest(const ChatRequest& request) {
  if (request.imagePrompt.empty() || request.imagePrompt.size() > 4000 || !g_utf8_validate(request.imagePrompt.c_str(), request.imagePrompt.size(), nullptr))
    return std::unexpected("image prompt must be UTF-8 text up to 4000 bytes");
  if (request.imagePrompt.find('\0') != std::string::npos) return std::unexpected("image prompt contains an invalid character");
  if (request.imageWidth < 256 || request.imageWidth > 768 || request.imageHeight < 256 || request.imageHeight > 768 || request.imageWidth % 64 || request.imageHeight % 64)
    return std::unexpected("image dimensions must be multiples of 64 between 256 and 768");
  if (request.imageSteps < 1 || request.imageSteps > 8) return std::unexpected("image steps must be between 1 and 8");
  if (request.imageSeed > 2147483647) return std::unexpected("image seed must be between 0 and 2147483647");
  if (!request.model.empty() && (request.model.size() > 200 || !request.model.ends_with(".gguf") || request.model.front() == '.' ||
      !std::ranges::all_of(request.model, [](unsigned char c) { return g_ascii_isalnum(c) || c == '.' || c == '-' || c == '_'; })))
    return std::unexpected("image model must be an installed GGUF file name");
  return {};
}

std::string encodePng(const std::vector<std::uint8_t>& rgb, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 || width > 768 || height > 768 || rgb.size() != static_cast<std::size_t>(width) * height * 3) return {};
  png_image image{};
  image.version = PNG_IMAGE_VERSION; image.width = width; image.height = height; image.format = PNG_FORMAT_RGB;
  png_alloc_size_t length = 0;
  if (!png_image_write_to_memory(&image, nullptr, &length, 0, rgb.data(), 0, nullptr) || length > kMaxGeneratedPng) { png_image_free(&image); return {}; }
  std::vector<std::uint8_t> bytes(length);
  if (!png_image_write_to_memory(&image, bytes.data(), &length, 0, rgb.data(), 0, nullptr)) { png_image_free(&image); return {}; }
  png_image_free(&image);
  gchar* encoded = g_base64_encode(bytes.data(), length);
  std::string out(encoded); g_free(encoded);
  return out;
}

ProcessImageGenerator::ProcessImageGenerator(fs::path worker, fs::path models) : worker_(std::move(worker)), models_(std::move(models)) {}

Outcome ProcessImageGenerator::generate(const ChatRequest& request, const ImageProgress& progress, const std::function<bool()>& cancelled) try {
  Outcome outcome;
  if (auto valid = validateImageRequest(request); !valid) { outcome.error = valid.error(); return outcome; }
  std::error_code ec;
  fs::path model;
  if (!request.model.empty()) model = models_ / request.model;
  else {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(models_, ec)) if (entry.is_regular_file(ec) && !entry.is_symlink(ec) && entry.path().extension() == ".gguf") files.push_back(entry.path());
    std::ranges::sort(files);
    if (!files.empty()) model = files.front();
  }
  if (model.empty() || !fs::is_regular_file(model, ec) || fs::is_symlink(model, ec)) { outcome.error = "no image model is installed; download SD-Turbo from the Models library"; return outcome; }
  if (!fs::is_regular_file(worker_, ec)) { outcome.error = "the image worker is not installed"; return outcome; }
  const std::string worker = worker_.string(), modelArg = model.string();
  const std::string input = nlohmann::json{{"prompt", request.imagePrompt}, {"width", request.imageWidth}, {"height", request.imageHeight}, {"steps", request.imageSteps}, {"seed", request.imageSeed}}.dump() + "\n";
  int in[2], out[2];
  if (::pipe2(in, O_CLOEXEC) != 0) { outcome.error = "could not create image worker input"; return outcome; }
  if (::pipe2(out, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); outcome.error = "could not create image worker output"; return outcome; }
  const pid_t child = ::fork();
  if (child == 0) {
    ::dup2(in[0], 0); ::dup2(out[1], 1);
    const char* argv[] = {worker.c_str(), "--model", modelArg.c_str(), nullptr};
    ::execv(worker.c_str(), const_cast<char* const*>(argv)); ::_exit(127);
  }
  ::close(in[0]); ::close(out[1]);
  if (child < 0) { ::close(in[1]); ::close(out[0]); outcome.error = "could not start the image worker"; return outcome; }
  struct Child {
    pid_t pid; int input, output;
    ~Child() { if (input >= 0) ::close(input); ::close(output); ::kill(pid, SIGKILL); int status; while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
  } process{child, in[1], out[0]};
  ::fcntl(process.input, F_SETFL, O_NONBLOCK); ::fcntl(process.output, F_SETFL, O_NONBLOCK);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  std::size_t written = 0, received = 0, frames = 0;
  std::string buffered;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancelled()) { outcome.cancelled = true; outcome.error = "cancelled"; return outcome; }
    pollfd fds[] = {{process.output, POLLIN, 0}, {process.input, POLLOUT, 0}};
    const int ready = ::poll(fds, 2, 100);
    if (ready < 0 && errno != EINTR) break;
    if (process.input >= 0 && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
      const auto count = ::write(process.input, input.data() + written, input.size() - written);
      if (count > 0) written += count;
      else if (count < 0 && errno != EAGAIN && errno != EINTR) { outcome.error = "image worker could not read the prompt"; return outcome; }
      if (written == input.size()) { ::close(process.input); process.input = -1; }
    }
    if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;
    std::array<char, 65536> buffer{};
    const auto count = ::read(process.output, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
    received += count;
    if (received > kMaxGeneratedPng * 2) { outcome.error = "image worker output exceeded its limit"; return outcome; }
    buffered.append(buffer.data(), count);
    for (auto newline = buffered.find('\n'); newline != std::string::npos; newline = buffered.find('\n')) {
      if (++frames > 100) { outcome.error = "image worker sent too many events"; return outcome; }
      auto frame = nlohmann::json::parse(buffered.substr(0, newline), nullptr, false);
      buffered.erase(0, newline + 1);
      if (!frame.is_object()) { outcome.error = "image worker returned invalid output"; return outcome; }
      if (frame.contains("progress")) {
        if (!frame["progress"].is_number() || !frame.value("stage", nlohmann::json()).is_string()) { outcome.error = "invalid image progress"; return outcome; }
        progress(std::clamp(frame["progress"].get<double>(), 0.0, 1.0), frame["stage"].get<std::string>().substr(0, 160));
        continue;
      }
      if (!frame.value("ok", false)) { outcome.error = frame.value("error", std::string("image generation failed")); return outcome; }
      const auto encoded = frame.value("png", std::string());
      if (encoded.size() > (kMaxGeneratedPng * 4 / 3 + 4)) { outcome.error = "generated image is too large"; return outcome; }
      gsize length = 0;
      guchar* png = g_base64_decode(encoded.c_str(), &length);
      auto decoded = decodeImageInput(png, length); g_free(png);
      if (!decoded || decoded->width != request.imageWidth || decoded->height != request.imageHeight) { outcome.error = "image worker returned an invalid PNG or dimensions"; return outcome; }
      outcome.ok = true; outcome.imagePng = encoded; outcome.imageModel = model.filename().string(); outcome.imagePrompt = request.imagePrompt;
      outcome.imageWidth = decoded->width; outcome.imageHeight = decoded->height; outcome.imageSeed = request.imageSeed;
      return outcome;
    }
  }
  outcome.error = "image worker stopped or exceeded its ten-minute deadline";
  return outcome;
} catch (const std::exception&) {
  Outcome outcome; outcome.error = "image worker returned invalid output"; return outcome;
}

Outcome FakeImageGenerator::generate(const ChatRequest& request, const ImageProgress& progress, const std::function<bool()>& cancelled) {
  Outcome outcome;
  const char* configured = std::getenv("HELIX_FAKE_IMAGE_DELAY_MS");
  const int delay = configured ? std::clamp(std::atoi(configured), 0, 1000) : 100;
  for (int i = 0; i <= 4; ++i) {
    if (cancelled()) { outcome.cancelled = true; outcome.error = "cancelled"; return outcome; }
    progress(i / 4.0, "Generating image"); std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(request.imageWidth) * request.imageHeight * 3);
  for (std::size_t at = 0; at < pixels.size(); at += 3) { pixels[at] = 80; pixels[at + 1] = 140; pixels[at + 2] = 200; }
  outcome.ok = true; outcome.imagePng = encodePng(pixels, request.imageWidth, request.imageHeight);
  outcome.imageModel = request.model.empty() ? "fake-image-model.gguf" : request.model; outcome.imagePrompt = request.imagePrompt; outcome.imageWidth = request.imageWidth; outcome.imageHeight = request.imageHeight; outcome.imageSeed = request.imageSeed;
  return outcome;
}

}  // namespace helix::inference
