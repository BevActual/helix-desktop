#include "system/desktop_ai.h"

#include <print>
#include <string>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "desktop_ai_parse_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace noctalia::desktop_ai;
  bool ok = true;

  const std::string chat = chatRequestJson("qwen3-4b", "Be brief.", "Hello");
  ok = expect(chat.find("\"model\":\"qwen3-4b\"") != std::string::npos, "chat model") && ok;
  ok = expect(chat.find("Be brief.") != std::string::npos && chat.find("Hello") != std::string::npos, "chat messages")
      && ok;

  const auto content = parseChatContent(R"({"choices":[{"message":{"content":"A short summary."}}]})");
  ok = expect(content && *content == "A short summary.", "chat content string") && ok;

  const auto parts = parseChatContent(
      R"({"choices":[{"message":{"content":[{"type":"text","text":"Hello "},{"type":"text","text":"there"}]}}]})"
  );
  ok = expect(parts && *parts == "Hello there", "chat content parts") && ok;
  ok = expect(!parseChatContent("{}").has_value(), "chat content missing") && ok;

  const auto transcript = parseTranscriptText(R"({"text":"good morning"})");
  ok = expect(transcript && *transcript == "good morning", "transcript text") && ok;

  ok = expect(parseErrorMessage(R"({"error":{"message":"no vision"}})") == "no vision", "error object") && ok;
  ok = expect(parseErrorMessage(R"({"error":"boom"})") == "boom", "error string") && ok;

  const std::string speech = speechRequestJson("tts-1", "Read this");
  ok = expect(
           speech.find("\"model\":\"tts-1\"") != std::string::npos && speech.find("\"response_format\":\"wav\"") != std::string::npos,
           "speech body"
       )
      && ok;

  const std::string vision = visionRequestJson("moondream", "Read this", "data:image/png;base64,aaaa");
  ok = expect(vision.find("image_url") != std::string::npos && vision.find("data:image/png;base64,aaaa") != std::string::npos, "vision body")
      && ok;

  return ok ? 0 : 1;
}
