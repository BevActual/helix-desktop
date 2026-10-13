#include "inference/transcriber.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace helix::inference {

namespace fs = std::filesystem;

ProcessTranscriber::ProcessTranscriber(fs::path worker, fs::path speechDir) : worker_(std::move(worker)), speechDir_(std::move(speechDir)) {}

namespace {

fs::path pickModel(const fs::path& dir) {
  if (const char* named = std::getenv("HELIX_SPEECH_MODEL"); named && *named) {
    const fs::path candidate = dir / fs::path(named).filename();  // a name, never a path chosen by the caller of this program
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
  }
  std::vector<fs::path> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file(ec) && name.rfind("ggml-", 0) == 0 && entry.path().extension() == ".bin") found.push_back(entry.path());
  }
  std::ranges::sort(found);
  return found.empty() ? fs::path{} : found.front();
}

struct Pipe {
  int in = -1, out = -1;
  ~Pipe() { if (in >= 0) ::close(in); if (out >= 0) ::close(out); }
};

}  // namespace

Outcome ProcessTranscriber::transcribe(const AudioInput& audio, const std::string& language, const std::function<bool()>& cancelled) {
  Outcome outcome;
  const fs::path model = pickModel(speechDir_);
  if (model.empty()) {
    outcome.error = "no speech model is installed; get one from the Models library";
    return outcome;
  }
  std::error_code ec;
  if (!fs::is_regular_file(worker_, ec)) {
    outcome.error = "the speech worker is not installed";
    return outcome;
  }
  if (audio.samples.empty() || audio.samples.size() > kMaxAudioSamples) {
    outcome.error = "the recording is empty or too long";
    return outcome;
  }

  int toChild[2], fromChild[2];
  if (::pipe2(toChild, O_CLOEXEC) != 0) { outcome.error = "could not start the speech worker"; return outcome; }
  if (::pipe2(fromChild, O_CLOEXEC) != 0) {
    ::close(toChild[0]); ::close(toChild[1]);
    outcome.error = "could not start the speech worker";
    return outcome;
  }
  const std::string modelArg = model.string();
  const std::string workerArg = worker_.string();
  const std::string languageArg = language;
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(toChild[0]); ::close(toChild[1]); ::close(fromChild[0]); ::close(fromChild[1]);
    outcome.error = "could not start the speech worker";
    return outcome;
  }
  if (child == 0) {
    ::dup2(toChild[0], 0);
    ::dup2(fromChild[1], 1);
    const char* argv[] = {workerArg.c_str(), "--model", modelArg.c_str(), "--language", languageArg.c_str(), nullptr};
    ::execv(workerArg.c_str(), const_cast<char* const*>(argv));
    ::_exit(127);
  }
  ::close(toChild[0]);
  ::close(fromChild[1]);
  Pipe pipes{toChild[1], fromChild[0]};
  ::fcntl(pipes.in, F_SETFL, O_NONBLOCK);
  ::fcntl(pipes.out, F_SETFL, O_NONBLOCK);

  // Input: a little-endian sample count, then the samples (this machine is little-endian; the worker checks the count).
  std::vector<std::uint8_t> input(4 + audio.samples.size() * 2);
  const auto count = static_cast<std::uint32_t>(audio.samples.size());
  std::memcpy(input.data(), &count, 4);
  std::memcpy(input.data() + 4, audio.samples.data(), audio.samples.size() * 2);

  const double seconds = static_cast<double>(audio.samples.size()) / kSampleRate;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<long>(60 + seconds * 4));
  std::size_t written = 0;
  std::string reply;
  bool killed = false;
  const auto kill = [&] { if (!killed) { ::kill(child, SIGKILL); killed = true; } };
  for (;;) {
    if (cancelled()) { kill(); outcome.cancelled = true; outcome.error = "cancelled"; break; }
    if (std::chrono::steady_clock::now() > deadline) { kill(); outcome.error = "transcription took too long"; break; }
    pollfd fds[2] = {{pipes.out, POLLIN, 0}, {written < input.size() ? pipes.in : -1, POLLOUT, 0}};
    const int ready = ::poll(fds, 2, 100);
    if (ready < 0 && errno != EINTR) { kill(); outcome.error = "the speech worker failed"; break; }
    if (written < input.size() && (fds[1].revents & POLLOUT)) {
      const ssize_t n = ::write(pipes.in, input.data() + written, std::min<std::size_t>(input.size() - written, 1 << 16));
      if (n > 0) written += static_cast<std::size_t>(n);
      if (written == input.size()) { ::close(pipes.in); pipes.in = -1; }
    }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      char buffer[4096];
      const ssize_t n = ::read(pipes.out, buffer, sizeof buffer);
      if (n > 0) { reply.append(buffer, static_cast<std::size_t>(n)); if (reply.size() > (1 << 20)) { kill(); outcome.error = "the speech worker sent too much"; break; } }
      else if (n == 0) break;  // the worker closed its output: it is done (or gone)
    }
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  if (!outcome.error.empty()) return outcome;
  try {
    const auto json = nlohmann::json::parse(reply);
    if (!json.value("ok", false)) { outcome.error = json.value("error", std::string("transcription failed")); return outcome; }
    outcome.ok = true;
    outcome.text = json.value("text", std::string());
    outcome.language = json.value("language", std::string());
  } catch (const std::exception&) {
    outcome.error = WIFSIGNALED(status) ? "the speech worker crashed" : "the speech worker gave no answer";
  }
  return outcome;
}

}  // namespace helix::inference
