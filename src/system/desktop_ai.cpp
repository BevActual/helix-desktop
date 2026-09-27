#include "system/desktop_ai.h"

#include "cli/schema_msg.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "net/http_client.h"
#include "pipewire/sound_player.h"
#include "system/ai_usage_service.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

namespace {

  constexpr Logger kLog("desktop-ai");
  constexpr std::size_t kMaxTextChars = 12000;
  constexpr std::size_t kMaxSpeechChars = 2000;
  constexpr std::uintmax_t kMaxImageBytes = 6 * 1024 * 1024;
  constexpr std::string_view kSummarizeSystem =
      "Summarize the user's text in a few sentences. Return only the summary.";
  constexpr std::string_view kTranslateSystem =
      "If the user's text is English, translate it to Spanish. Otherwise translate it to English. Return only the translation.";
  constexpr std::string_view kRewriteSystem =
      "Rewrite the user's text so it is clearer and more concise. Return only the rewritten text.";
  constexpr std::string_view kReadPrompt =
      "Read all visible text in this image. Return only that text, keeping the line breaks.";
  constexpr std::string_view kDescribePrompt = "Describe this screenshot in two or three sentences.";

  [[nodiscard]] std::string base64Encode(std::string_view bytes) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
      const unsigned value = (static_cast<unsigned char>(bytes[i]) << 16)
          | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
      out.push_back(kTable[(value >> 18) & 63]);
      out.push_back(kTable[(value >> 12) & 63]);
      out.push_back(kTable[(value >> 6) & 63]);
      out.push_back(kTable[value & 63]);
      i += 3;
    }
    if (i < bytes.size()) {
      unsigned value = static_cast<unsigned char>(bytes[i]) << 16;
      out.push_back(kTable[(value >> 18) & 63]);
      if (i + 1 < bytes.size()) {
        value |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out.push_back(kTable[(value >> 12) & 63]);
        out.push_back(kTable[(value >> 6) & 63]);
        out.push_back('=');
      } else {
        out.push_back(kTable[(value >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
      }
    }
    return out;
  }

  [[nodiscard]] std::string imageMime(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& ch : ext) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
    if (ext == ".jpg" || ext == ".jpeg") {
      return "image/jpeg";
    }
    if (ext == ".webp") {
      return "image/webp";
    }
    if (ext == ".gif") {
      return "image/gif";
    }
    return "image/png";
  }

  [[nodiscard]] std::string readFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      error = "unreadable";
      return {};
    }
    std::string bytes;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) {
      error = "unreadable";
      return {};
    }
    bytes.resize(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
      in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!in) {
      error = "unreadable";
      return {};
    }
    return bytes;
  }

  [[nodiscard]] std::string multipartTranscription(std::string_view model, std::string_view filename, std::string_view bytes) {
    constexpr std::string_view kBoundary = "----HelixDesktopAi";
    std::string body;
    body.reserve(bytes.size() + 256);
    body += "--";
    body += kBoundary;
    body += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    body += model;
    body += "\r\n--";
    body += kBoundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    body += filename;
    body += "\"\r\nContent-Type: audio/wav\r\n\r\n";
    body += bytes;
    body += "\r\n--";
    body += kBoundary;
    body += "--\r\n";
    return body;
  }

  [[nodiscard]] std::optional<std::vector<std::string>> dictateCommand(const std::filesystem::path& wav) {
    const std::string path = wav.string();
    if (process::commandExists("pw-record")) {
      return std::vector<std::string>{"pw-record", "--rate", "16000", "--channels", "1", "--format", "s16", path};
    }
    if (process::commandExists("parecord")) {
      return std::vector<std::string>{
          "parecord", "--file-format=wav", "--rate=16000", "--channels=1", "--format=s16le", path
      };
    }
    if (process::commandExists("ffmpeg")) {
      return std::vector<std::string>{
          "ffmpeg", "-y", "-f", "pulse", "-i", "default", "-ac", "1", "-ar", "16000", path
      };
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string clip(std::string text, std::size_t maxChars) {
    text = StringUtils::trim(text);
    if (text.size() > maxChars) {
      text.resize(maxChars);
    }
    return text;
  }

  [[nodiscard]] std::string needsKey(noctalia::ai_usage::LocalAiCapability capability) {
    using noctalia::ai_usage::LocalAiCapability;
    switch (capability) {
    case LocalAiCapability::Vision:
      return "desktop-ai.needs-vision";
    case LocalAiCapability::Transcript:
      return "desktop-ai.needs-transcript";
    case LocalAiCapability::Tts:
      return "desktop-ai.needs-tts";
    default:
      return "desktop-ai.needs-chat";
    }
  }

} // namespace

namespace noctalia::desktop_ai {

  std::string chatRequestJson(std::string_view model, std::string_view system, std::string_view user) {
    nlohmann::json body = {
        {"model", model},
        {"temperature", 0.2},
        {"messages",
         nlohmann::json::array({
             {{"role", "system"}, {"content", system}},
             {{"role", "user"}, {"content", user}},
         })},
    };
    return body.dump();
  }

  std::string visionRequestJson(std::string_view model, std::string_view prompt, std::string_view dataUri) {
    nlohmann::json body = {
        {"model", model},
        {"temperature", 0.2},
        {"messages",
         nlohmann::json::array({
             {{"role", "user"},
              {"content",
               nlohmann::json::array({
                   {{"type", "text"}, {"text", prompt}},
                   {{"type", "image_url"}, {"image_url", {{"url", dataUri}}}},
               })}},
         })},
    };
    return body.dump();
  }

  std::string speechRequestJson(std::string_view model, std::string_view text) {
    nlohmann::json body = {
        {"model", model},
        {"input", text},
        {"response_format", "wav"},
    };
    return body.dump();
  }

  std::optional<std::string> parseChatContent(std::string_view json) {
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return std::nullopt;
    }
    const auto choices = parsed.find("choices");
    if (choices == parsed.end() || !choices->is_array() || choices->empty()) {
      return std::nullopt;
    }
    const auto message = (*choices)[0].find("message");
    if (message == (*choices)[0].end() || !message->is_object()) {
      return std::nullopt;
    }
    const auto content = message->find("content");
    if (content == message->end() || content->is_null()) {
      return std::nullopt;
    }
    if (content->is_string()) {
      return content->get<std::string>();
    }
    if (!content->is_array()) {
      return std::nullopt;
    }
    std::string joined;
    for (const auto& part : *content) {
      if (part.is_string()) {
        joined += part.get<std::string>();
      } else if (part.is_object()) {
        const auto text = part.find("text");
        if (text != part.end() && text->is_string()) {
          joined += text->get<std::string>();
        }
      }
    }
    if (joined.empty()) {
      return std::nullopt;
    }
    return joined;
  }

  std::optional<std::string> parseTranscriptText(std::string_view json) {
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return std::nullopt;
    }
    const auto text = parsed.find("text");
    if (text == parsed.end() || !text->is_string()) {
      return std::nullopt;
    }
    return text->get<std::string>();
  }

  std::string parseErrorMessage(std::string_view json) {
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return {};
    }
    const auto error = parsed.find("error");
    if (error == parsed.end()) {
      return {};
    }
    if (error->is_string()) {
      return error->get<std::string>();
    }
    if (error->is_object()) {
      const auto message = error->find("message");
      if (message != error->end() && message->is_string()) {
        return message->get<std::string>();
      }
    }
    return {};
  }

} // namespace noctalia::desktop_ai

DesktopAiService::DesktopAiService(AiUsageService& usage, HttpClient& http, ClipboardService& clipboard)
    : m_usage(usage), m_http(http), m_clipboard(clipboard) {}

DesktopAiService::~DesktopAiService() {
  ++m_generation;
  m_state.recording = false;
  if (m_dictateCancel) {
    m_dictateCancel->store(true);
  }
}

void DesktopAiService::addChangeCallback(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

void DesktopAiService::notify() {
  for (const auto& callback : m_callbacks) {
    if (callback) {
      callback();
    }
  }
}

std::optional<DesktopAiService::Endpoint>
DesktopAiService::resolve(noctalia::ai_usage::LocalAiCapability capability) const {
  const auto& local = m_usage.snapshot().localAi;
  if (local.status != noctalia::ai_usage::LocalAiStatus::Running || local.baseUrl.empty()) {
    return std::nullopt;
  }
  const auto* model = noctalia::ai_usage::localAiModelFor(local, capability);
  if (model == nullptr || model->id.empty()) {
    return std::nullopt;
  }
  Endpoint endpoint;
  endpoint.baseUrl = local.baseUrl;
  while (!endpoint.baseUrl.empty() && endpoint.baseUrl.back() == '/') {
    endpoint.baseUrl.pop_back();
  }
  endpoint.model = model->id;
  return endpoint;
}

std::string DesktopAiService::modelReason(noctalia::ai_usage::LocalAiCapability capability) const {
  const auto& local = m_usage.snapshot().localAi;
  if (local.status != noctalia::ai_usage::LocalAiStatus::Running || local.baseUrl.empty()) {
    return i18n::tr("desktop-ai.stopped");
  }
  if (!resolve(capability).has_value()) {
    return i18n::tr(needsKey(capability));
  }
  return {};
}

std::string DesktopAiService::unavailableReason(noctalia::ai_usage::LocalAiCapability capability) const {
  if (m_state.busy) {
    return i18n::tr("desktop-ai.busy");
  }
  return modelReason(capability);
}

std::string DesktopAiService::clipboardText() const {
  const auto text = m_clipboard.clipboardText();
  return text.has_value() ? clip(*text, kMaxTextChars) : std::string{};
}

bool DesktopAiService::claim(DesktopAiSurface surface, DesktopAiTask task, std::string status) {
  if (m_state.busy || m_state.recording) {
    m_state.failed = true;
    m_state.surface = surface;
    m_state.status = i18n::tr("desktop-ai.busy");
    notify();
    return false;
  }
  ++m_generation;
  m_state.busy = true;
  m_state.failed = false;
  m_state.task = task;
  m_state.surface = surface;
  m_state.status = std::move(status);
  m_state.result.clear();
  notify();
  return true;
}

void DesktopAiService::fail(DesktopAiSurface surface, std::string status) {
  m_state.busy = false;
  m_state.recording = false;
  m_state.failed = true;
  m_state.surface = surface;
  m_state.status = std::move(status);
  m_state.result.clear();
  notify();
}

void DesktopAiService::succeed(std::string result, bool copyToClipboard) {
  result = StringUtils::trim(result);
  m_state.busy = false;
  m_state.recording = false;
  m_state.failed = result.empty();
  m_state.result = std::move(result);
  if (m_state.result.empty()) {
    m_state.status = i18n::tr("desktop-ai.empty");
  } else if (copyToClipboard && m_clipboard.copyText(m_state.result)) {
    m_state.status = i18n::tr("desktop-ai.copied");
  } else {
    m_state.status = i18n::tr("desktop-ai.done");
  }
  notify();
}

void DesktopAiService::postChat(
    noctalia::ai_usage::LocalAiCapability capability, std::string system, std::string user, bool copyToClipboard
) {
  const auto endpoint = resolve(capability);
  if (!endpoint.has_value()) {
    fail(m_state.surface, modelReason(capability));
    return;
  }
  const std::uint64_t generation = m_generation;
  const std::string url = endpoint->baseUrl + "/v1/chat/completions";
  HttpRequest request;
  request.method = "POST";
  request.url = url;
  request.headers = {"Content-Type: application/json", "Accept: application/json"};
  request.body = noctalia::desktop_ai::chatRequestJson(endpoint->model, system, user);
  request.allowWhenOffline = true;
  request.timeoutSeconds = 120;
  m_http.request(std::move(request), [this, generation, copyToClipboard](HttpResponse response) {
    if (generation != m_generation) {
      return;
    }
    if (!response.transportOk || response.status < 200 || response.status >= 300) {
      std::string message = noctalia::desktop_ai::parseErrorMessage(response.body);
      if (message.empty()) {
        message = i18n::tr("desktop-ai.failed");
      }
      fail(m_state.surface, std::move(message));
      return;
    }
    const auto content = noctalia::desktop_ai::parseChatContent(response.body);
    if (!content.has_value()) {
      fail(m_state.surface, i18n::tr("desktop-ai.failed"));
      return;
    }
    succeed(*content, copyToClipboard);
  });
}

void DesktopAiService::summarize(std::string text) {
  text = clip(std::move(text), kMaxTextChars);
  if (text.empty()) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.empty"));
    return;
  }
  if (const std::string reason = modelReason(noctalia::ai_usage::LocalAiCapability::Chat); !reason.empty()) {
    fail(DesktopAiSurface::Clipboard, reason);
    return;
  }
  if (!claim(DesktopAiSurface::Clipboard, DesktopAiTask::Summarize, i18n::tr("desktop-ai.working"))) {
    return;
  }
  postChat(noctalia::ai_usage::LocalAiCapability::Chat, std::string(kSummarizeSystem), std::move(text), true);
}

void DesktopAiService::translate(std::string text) {
  text = clip(std::move(text), kMaxTextChars);
  if (text.empty()) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.empty"));
    return;
  }
  if (const std::string reason = modelReason(noctalia::ai_usage::LocalAiCapability::Chat); !reason.empty()) {
    fail(DesktopAiSurface::Clipboard, reason);
    return;
  }
  if (!claim(DesktopAiSurface::Clipboard, DesktopAiTask::Translate, i18n::tr("desktop-ai.working"))) {
    return;
  }
  postChat(noctalia::ai_usage::LocalAiCapability::Chat, std::string(kTranslateSystem), std::move(text), true);
}

void DesktopAiService::rewrite(std::string text) {
  text = clip(std::move(text), kMaxTextChars);
  if (text.empty()) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.empty"));
    return;
  }
  if (const std::string reason = modelReason(noctalia::ai_usage::LocalAiCapability::Chat); !reason.empty()) {
    fail(DesktopAiSurface::Clipboard, reason);
    return;
  }
  if (!claim(DesktopAiSurface::Clipboard, DesktopAiTask::Rewrite, i18n::tr("desktop-ai.working"))) {
    return;
  }
  postChat(noctalia::ai_usage::LocalAiCapability::Chat, std::string(kRewriteSystem), std::move(text), true);
}

void DesktopAiService::summarizeClipboard() { summarize(clipboardText()); }

void DesktopAiService::translateClipboard() { translate(clipboardText()); }

void DesktopAiService::rewriteClipboard() { rewrite(clipboardText()); }

void DesktopAiService::speakClipboard() { speak(clipboardText()); }

void DesktopAiService::speak(std::string text) {
  text = clip(std::move(text), kMaxSpeechChars);
  if (text.empty()) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.empty"));
    return;
  }
  if (const std::string reason = modelReason(noctalia::ai_usage::LocalAiCapability::Tts); !reason.empty()) {
    fail(DesktopAiSurface::Clipboard, reason);
    return;
  }
  if (!claim(DesktopAiSurface::Clipboard, DesktopAiTask::Speak, i18n::tr("desktop-ai.speaking"))) {
    return;
  }
  postSpeech(std::move(text));
}

void DesktopAiService::postSpeech(std::string text) {
  const auto endpoint = resolve(noctalia::ai_usage::LocalAiCapability::Tts);
  if (!endpoint.has_value()) {
    fail(DesktopAiSurface::Clipboard, modelReason(noctalia::ai_usage::LocalAiCapability::Tts));
    return;
  }
  const std::uint64_t generation = m_generation;
  HttpRequest request;
  request.method = "POST";
  request.url = endpoint->baseUrl + "/v1/audio/speech";
  request.headers = {"Content-Type: application/json", "Accept: audio/wav"};
  request.body = noctalia::desktop_ai::speechRequestJson(endpoint->model, text);
  request.allowWhenOffline = true;
  request.timeoutSeconds = 90;
  m_http.request(std::move(request), [this, generation](HttpResponse response) {
    if (generation != m_generation) {
      return;
    }
    const bool audio = response.transportOk && response.status >= 200 && response.status < 300 && !response.body.empty()
        && response.body.front() != '{';
    if (!audio) {
      std::string message = noctalia::desktop_ai::parseErrorMessage(response.body);
      fail(DesktopAiSurface::Clipboard, message.empty() ? i18n::tr("desktop-ai.failed") : std::move(message));
      return;
    }
    const auto path = std::filesystem::temp_directory_path() / "helix-desktop-ai-speech.wav";
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
      if (!out) {
        fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.failed"));
        return;
      }
    }
    if (m_soundPlayer != nullptr) {
      m_soundPlayer->playFile(path);
    } else if (process::commandExists("pw-play")) {
      (void)process::runAsync(std::vector<std::string>{"pw-play", path.string()});
    } else if (process::commandExists("paplay")) {
      (void)process::runAsync(std::vector<std::string>{"paplay", path.string()});
    }
    m_state.busy = false;
    m_state.failed = false;
    m_state.status = i18n::tr("desktop-ai.done");
    notify();
  });
}

void DesktopAiService::describeImage(std::string path) {
  if (const std::string reason = modelReason(noctalia::ai_usage::LocalAiCapability::Vision); !reason.empty()) {
    fail(DesktopAiSurface::Capture, reason);
    return;
  }
  if (!claim(DesktopAiSurface::Capture, DesktopAiTask::Describe, i18n::tr("desktop-ai.working"))) {
    return;
  }
  postVision(std::move(path), std::string(kDescribePrompt));
}

void DesktopAiService::readImage(std::string path) {
  if (const std::string reason = modelReason(noctalia::ai_usage::LocalAiCapability::Vision); !reason.empty()) {
    fail(DesktopAiSurface::Capture, reason);
    return;
  }
  if (!claim(DesktopAiSurface::Capture, DesktopAiTask::Read, i18n::tr("desktop-ai.working"))) {
    return;
  }
  postVision(std::move(path), std::string(kReadPrompt));
}

void DesktopAiService::postVision(std::string path, std::string prompt) {
  const auto endpoint = resolve(noctalia::ai_usage::LocalAiCapability::Vision);
  if (!endpoint.has_value()) {
    fail(DesktopAiSurface::Capture, modelReason(noctalia::ai_usage::LocalAiCapability::Vision));
    return;
  }
  std::error_code ec;
  const auto file = std::filesystem::path(path);
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size == 0 || size > kMaxImageBytes) {
    fail(DesktopAiSurface::Capture, i18n::tr(size > kMaxImageBytes ? "desktop-ai.too-large" : "desktop-ai.failed"));
    return;
  }
  std::string readError;
  const std::string bytes = readFile(file, readError);
  if (!readError.empty() || bytes.empty()) {
    fail(DesktopAiSurface::Capture, i18n::tr("desktop-ai.failed"));
    return;
  }
  const std::string dataUri = "data:" + imageMime(file) + ";base64," + base64Encode(bytes);
  const std::uint64_t generation = m_generation;
  HttpRequest request;
  request.method = "POST";
  request.url = endpoint->baseUrl + "/v1/chat/completions";
  request.headers = {"Content-Type: application/json", "Accept: application/json"};
  request.body = noctalia::desktop_ai::visionRequestJson(endpoint->model, prompt, dataUri);
  request.allowWhenOffline = true;
  request.timeoutSeconds = 180;
  m_http.request(std::move(request), [this, generation](HttpResponse response) {
    if (generation != m_generation) {
      return;
    }
    if (!response.transportOk || response.status < 200 || response.status >= 300) {
      std::string message = noctalia::desktop_ai::parseErrorMessage(response.body);
      fail(DesktopAiSurface::Capture, message.empty() ? i18n::tr("desktop-ai.failed") : std::move(message));
      return;
    }
    const auto content = noctalia::desktop_ai::parseChatContent(response.body);
    if (!content.has_value()) {
      fail(DesktopAiSurface::Capture, i18n::tr("desktop-ai.failed"));
      return;
    }
    succeed(*content, true);
  });
}

void DesktopAiService::toggleDictation() {
  if (m_state.recording) {
    if (m_dictateCancel) {
      m_dictateCancel->store(true);
    }
    m_state.status = i18n::tr("desktop-ai.dictate-stop");
    notify();
    return;
  }
  if (m_state.busy) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.busy"));
    return;
  }
  if (const std::string reason = unavailableReason(noctalia::ai_usage::LocalAiCapability::Transcript); !reason.empty()) {
    fail(DesktopAiSurface::Clipboard, reason);
    return;
  }
  const auto path = std::filesystem::temp_directory_path() / "helix-desktop-ai-dictate.wav";
  const auto command = dictateCommand(path);
  if (!command.has_value()) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.dictate-missing"));
    return;
  }
  ++m_generation;
  const std::uint64_t generation = m_generation;
  m_dictateCancel = std::make_shared<std::atomic<bool>>(false);
  m_state.recording = true;
  m_state.failed = false;
  m_state.busy = false;
  m_state.task = DesktopAiTask::Dictate;
  m_state.surface = DesktopAiSurface::Clipboard;
  m_state.status = i18n::tr("desktop-ai.dictate-start");
  m_state.result.clear();
  notify();
  process::RunOptions options;
  options.timeout = std::chrono::seconds(60);
  options.cancel = m_dictateCancel;
  options.maxOutputBytes = 0;
  const bool launched = process::runAsync(
      *command,
      process::RunCallbacks{
          .onExit =
              [this, generation, path = path.string()](process::RunResult) {
                DeferredCall::callLater([this, generation, path]() { finishDictation(generation, path); });
              },
      },
      options
  );
  if (!launched) {
    m_state.recording = false;
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.dictate-missing"));
  }
}

void DesktopAiService::finishDictation(std::uint64_t generation, std::string path) {
  if (generation != m_generation || !m_state.recording) {
    return;
  }
  m_state.recording = false;
  transcribeFile(std::move(path));
}

void DesktopAiService::transcribeFile(std::string path) {
  const auto endpoint = resolve(noctalia::ai_usage::LocalAiCapability::Transcript);
  if (!endpoint.has_value()) {
    fail(DesktopAiSurface::Clipboard, i18n::tr(needsKey(noctalia::ai_usage::LocalAiCapability::Transcript)));
    return;
  }
  std::string readError;
  const std::string bytes = readFile(path, readError);
  if (!readError.empty() || bytes.size() < 64) {
    fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.empty"));
    return;
  }
  if (!claim(DesktopAiSurface::Clipboard, DesktopAiTask::Dictate, i18n::tr("desktop-ai.dictate-stop"))) {
    return;
  }
  const std::uint64_t generation = m_generation;
  HttpRequest request;
  request.method = "POST";
  request.url = endpoint->baseUrl + "/v1/audio/transcriptions";
  request.headers = {"Content-Type: multipart/form-data; boundary=----HelixDesktopAi", "Accept: application/json"};
  request.body = multipartTranscription(endpoint->model, "dictate.wav", bytes);
  request.allowWhenOffline = true;
  request.timeoutSeconds = 180;
  m_http.request(std::move(request), [this, generation](HttpResponse response) {
    if (generation != m_generation) {
      return;
    }
    if (!response.transportOk || response.status < 200 || response.status >= 300) {
      std::string message = noctalia::desktop_ai::parseErrorMessage(response.body);
      fail(DesktopAiSurface::Clipboard, message.empty() ? i18n::tr("desktop-ai.failed") : std::move(message));
      return;
    }
    const auto text = noctalia::desktop_ai::parseTranscriptText(response.body);
    if (!text.has_value()) {
      fail(DesktopAiSurface::Clipboard, i18n::tr("desktop-ai.failed"));
      return;
    }
    succeed(*text, true);
  });
}

void DesktopAiService::registerIpc(IpcService& ipc) {
  using noctalia::cli::msg::desktopAiDescribe;
  using noctalia::cli::msg::desktopAiDictate;
  using noctalia::cli::msg::desktopAiRead;
  using noctalia::cli::msg::desktopAiRewrite;
  using noctalia::cli::msg::desktopAiSpeak;
  using noctalia::cli::msg::desktopAiStatus;
  using noctalia::cli::msg::desktopAiSummarize;
  using noctalia::cli::msg::desktopAiTranslate;

  ipc.bind(desktopAiStatus, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: desktop-ai-status takes no arguments\n";
    }
    std::string line = m_state.recording ? "recording" : (m_state.busy ? "busy" : (m_state.failed ? "error" : "idle"));
    line += " ";
    line += m_state.status;
    if (!m_state.result.empty()) {
      line += "\n";
      line += m_state.result;
    }
    line += "\n";
    return line;
  });
  ipc.bind(desktopAiSummarize, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: desktop-ai-summarize takes no arguments\n";
    }
    summarizeClipboard();
    return "ok\n";
  });
  ipc.bind(desktopAiTranslate, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: desktop-ai-translate takes no arguments\n";
    }
    translateClipboard();
    return "ok\n";
  });
  ipc.bind(desktopAiRewrite, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: desktop-ai-rewrite takes no arguments\n";
    }
    rewriteClipboard();
    return "ok\n";
  });
  ipc.bind(desktopAiSpeak, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: desktop-ai-speak takes no arguments\n";
    }
    speakClipboard();
    return "ok\n";
  });
  ipc.bind(desktopAiDictate, [this](const std::string& args) -> std::string {
    if (!noctalia::ipc::splitWords(args).empty()) {
      return "error: desktop-ai-dictate takes no arguments\n";
    }
    toggleDictation();
    return "ok\n";
  });
  const auto pathCommand = [this](auto&& run, const char* usage) {
    return [this, run = std::move(run), usage](const std::string& args) -> std::string {
      const std::string path = StringUtils::trim(args);
      if (path.empty()) {
        return std::string("error: ") + usage + "\n";
      }
      run(path);
      return "ok\n";
    };
  };
  ipc.bind(
      desktopAiDescribe,
      pathCommand([this](const std::string& path) { describeImage(path); }, "desktop-ai-describe requires a file path")
  );
  ipc.bind(
      desktopAiRead,
      pathCommand([this](const std::string& path) { readImage(path); }, "desktop-ai-read requires a file path")
  );
}
