#include "net/localsend_protocol.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <system_error>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "localsend_protocol_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace localsend;
  bool ok = true;

  DeviceInfo info;
  info.alias = "Nice Orange";
  info.fingerprint = "abc";
  info.port = 53317;
  info.protocol = "http";
  info.announce = true;
  const std::string json = serializeDeviceJson(info, true);
  const auto parsed = parseDeviceJson(json);
  ok = expect(parsed.has_value(), "parse announcement") && ok;
  ok = expect(parsed && parsed->alias == "Nice Orange", "alias") && ok;
  ok = expect(parsed && parsed->announce, "announce flag") && ok;
  ok = expect(parsed && parsed->protocol == "http", "protocol") && ok;

  ok = expect(!parseDeviceJson("{").has_value(), "reject truncated json") && ok;
  ok = expect(!parseDeviceJson("{}").has_value(), "reject empty device") && ok;

  PrepareUploadRequest request;
  request.info = info;
  request.files.push_back(FileMeta{.id = "1", .fileName = "photo.png", .size = 12, .fileType = "image/png"});
  const auto prepJson = serializePrepareUploadRequestJson(request);
  const auto prep = parsePrepareUploadJson(prepJson);
  ok = expect(prep.has_value() && prep->files.size() == 1, "prepare files") && ok;
  ok = expect(prep && prep->files.front().fileName == "photo.png", "prepare file name") && ok;

  PrepareUploadResponse response;
  response.sessionId = "sess";
  response.fileTokens["1"] = "tok";
  const auto responseParsed = parsePrepareUploadResponseJson(serializePrepareUploadResponseJson(response));
  ok = expect(responseParsed && responseParsed->sessionId == "sess", "session id") && ok;
  ok = expect(responseParsed && responseParsed->fileTokens.at("1") == "tok", "file token") && ok;

  const auto query = parseQuery("sessionId=s%201&fileId=a&token=b&pin=12+3");
  ok = expect(query.sessionId == "s 1", "query session decode") && ok;
  ok = expect(query.fileId == "a" && query.token == "b", "query ids") && ok;
  ok = expect(query.pin == "12 3", "query pin plus") && ok;

  ok = expect(sanitizeFileName("../../etc/passwd") == "passwd", "sanitize parent") && ok;
  ok = expect(sanitizeFileName("") == "file", "sanitize empty") && ok;
  ok = expect(sanitizeFileName("..") == "file", "sanitize dots") && ok;
  ok = expect(mimeFromFileName("x.PNG") == "image/png", "mime png") && ok;
  ok = expect(mimeFromFileName("note") == "application/octet-stream", "mime fallback") && ok;
  ok = expect(isLocalSendApiPath("/api/localsend/v2/info", "/info"), "v2 info path") && ok;
  ok = expect(isLocalSendApiPath("/api/localsend/v1/register", "/register"), "v1 register path") && ok;
  ok = expect(!isLocalSendApiPath("/api/localsend/v2/upload", "/info"), "route mismatch") && ok;
  ok = expect(peerKey("1.2.3.4", 53317, "fp") == "fp", "fingerprint key") && ok;
  ok = expect(peerKey("1.2.3.4", 9, "") == "1.2.3.4:9", "ip port key") && ok;

  {
    const auto root = std::filesystem::temp_directory_path() / "noctalia-localsend-collect-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "sub" / "deep" / "deeper", ec);
    std::ofstream(root / "a.txt") << "a";
    std::ofstream(root / ".hidden") << "h";
    std::ofstream(root / "sub" / "b.txt") << "b";
    std::ofstream(root / "sub" / "deep" / "c.txt") << "c";
    std::ofstream(root / "sub" / "deep" / "deeper" / "d.txt") << "d";
    const auto collected = collectSendFiles(root);
    ok = expect(collected.files.size() == 3, "collect depth and hidden") && ok;
    ok = expect(!collected.truncated, "collect not truncated") && ok;
    const auto single = collectSendFiles(root / "a.txt");
    ok = expect(single.files.size() == 1 && single.files.front().filename() == "a.txt", "collect file") && ok;
    std::filesystem::remove_all(root, ec);
  }

  return ok ? 0 : 1;
}
