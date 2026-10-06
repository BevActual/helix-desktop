#include "shell/agent/agent_confirmation_queue.h"
#include "tests/test_check.h"

using namespace helix::agent;
using nlohmann::json;

namespace {

struct Answer { std::uint64_t operation; bool approved; std::vector<std::string> tiers; };

ConfirmRequest action(std::uint64_t operation, const std::string& client = "exe:/usr/bin/agent") {
  ConfirmRequest request;
  request.operation = operation;
  request.client = client;
  request.tool = "app-launch";
  return request;
}

void checkSanitize() {
  TEST_CHECK(sanitizeForDisplay("plain text", 50) == "plain text");
  TEST_CHECK(sanitizeForDisplay("two\nlines\tand\rmore", 50) == "two lines and more");  // no faked extra lines
  TEST_CHECK(sanitizeForDisplay(std::string("a\x1b[31mb\x07", 7), 50) == "a[31mb");        // escapes and bells dropped
  TEST_CHECK(sanitizeForDisplay("safe\xE2\x80\xAEtxt.exe", 50) == "safetxt.exe");          // right-to-left override removed
  TEST_CHECK(sanitizeForDisplay("zero\xE2\x80\x8Bwidth\xEF\xBB\xBF", 50) == "zerowidth");
  TEST_CHECK(sanitizeForDisplay("caf\xC3\xA9 \xE2\x9C\x93", 50) == "caf\xC3\xA9 \xE2\x9C\x93");  // real text survives
  TEST_CHECK(sanitizeForDisplay("bad\xFF" "byte", 50) == "bad?byte");
  TEST_CHECK(sanitizeForDisplay("over\xC0\xAFlong", 50).find('/') == std::string::npos);        // overlong '/' is not '/'
  TEST_CHECK(sanitizeForDisplay("\xED\xA0\x80", 50) == "?");                                      // surrogate
  TEST_CHECK(sanitizeForDisplay("truncated\xE2\x82", 50) == "truncated??");                       // each invalid byte is one "?"
  TEST_CHECK(sanitizeForDisplay("abcdefghij", 4) == "abcd…");
  TEST_CHECK(sanitizeForDisplay("\xC3\xA9\xC3\xA9\xC3\xA9", 2) == "\xC3\xA9\xC3\xA9…");           // counts characters, not bytes
}

void checkFriendlyName() {
  using helix::agent::friendlyClientName;
  TEST_CHECK(friendlyClientName("exe:/usr/bin/helix-settings") == "helix-settings");
  TEST_CHECK(friendlyClientName("exe:/usr/bin/python3.14#/home/u/tools/server.py") == "server.py");
  // Hostile names cannot read as sentences, hide behind bidi controls or borrow look-alike letters.
  TEST_CHECK(friendlyClientName(std::string("exe:/tmp/evil\xE2\x80\xAE" "exe.txt\nSettings: all access granted")) == "evil___exe.txt_Settings__all_access_gran...");  // capped at 40 characters
  TEST_CHECK(friendlyClientName("exe:/tmp/Settings (trusted)") == "Settings__trusted_");
  TEST_CHECK(friendlyClientName("exe:/tmp/\xD0\xA1ontrol") == "__ontrol");  // Cyrillic 'S' is not a Latin 'C'
  TEST_CHECK(friendlyClientName("exe:/usr/bin/" + std::string(100, 'a')).size() <= 43);
  TEST_CHECK(friendlyClientName("") == "unknown" && friendlyClientName("exe:/") == "unknown");
}

void checkParse() {
  auto request = ConfirmRequest::fromJson(json{{"operation", 7}, {"client", "exe:/x"}, {"tool", "app-launch"},
    {"args", {"Settings"}}, {"reason", "open it\nnow"}, {"tainted", true}});
  TEST_CHECK(request && request->operation == 7 && request->kind == ConfirmRequest::Kind::Action);
  TEST_CHECK(request->reason == "open it now" && request->tainted && request->args == std::vector<std::string>{"Settings"});

  auto grant = ConfirmRequest::fromJson(json{{"operation", 8}, {"kind", "grant"}, {"client", "exe:/x"}, {"label", "My Agent"},
    {"tiers", {"observe", "act"}}});
  TEST_CHECK(grant && grant->kind == ConfirmRequest::Kind::Grant && grant->tiers.size() == 2 && grant->label == "My Agent");

  // Malformed requests are rejected rather than shown half-empty.
  TEST_CHECK(!ConfirmRequest::fromJson(json::array()));
  TEST_CHECK(!ConfirmRequest::fromJson(json{{"client", "exe:/x"}, {"tool", "t"}}));                    // no operation
  TEST_CHECK(!ConfirmRequest::fromJson(json{{"operation", 1}, {"tool", "t"}}));                        // no client
  TEST_CHECK(!ConfirmRequest::fromJson(json{{"operation", 1}, {"client", "c"}}));                      // action without tool
  TEST_CHECK(!ConfirmRequest::fromJson(json{{"operation", 1}, {"client", "c"}, {"kind", "grant"}}));   // grant without tiers

  // Size limits: a huge argument list or long values cannot grow the dialog without bound.
  json many = json::array();
  for (int i = 0; i < 40; ++i) many.push_back(std::string(500, 'x'));
  auto big = ConfirmRequest::fromJson(json{{"operation", 1}, {"client", "c"}, {"tool", "t"}, {"args", many}});
  TEST_CHECK(big && big->args.size() == 8 && big->args[0].size() <= 124);
}

void checkClientName() {
  // A long identity is shortened for display, but the name still comes from its end, not from the cut-off point.
  const std::string long_id = "exe:/usr/bin/python3.14#/home/someone/a/very/long/path/" + std::string(300, 'd') + "/server.py";
  auto request = ConfirmRequest::fromJson(json{{"operation", 1}, {"client", long_id}, {"tool", "status"}});
  TEST_CHECK(request && request->clientName == "server.py");
  TEST_CHECK(request->client.size() < long_id.size());  // shortened with an ellipsis
}

void checkQueue() {
  std::vector<Answer> answers;
  ConfirmationQueue queue([&](std::uint64_t operation, bool approved, const std::vector<std::string>& tiers) {
    answers.push_back({operation, approved, tiers});
  });
  int changes = 0;
  queue.onChanged = [&] { ++changes; };

  queue.enqueue(action(1));
  TEST_CHECK(changes == 1 && queue.front() && queue.front()->operation == 1);
  queue.enqueue(action(2, "exe:/other"));
  TEST_CHECK(changes == 1 && queue.size() == 2);  // the shown request did not change

  queue.answerFront(true);
  TEST_CHECK(answers.size() == 1 && answers[0].operation == 1 && answers[0].approved);
  TEST_CHECK(queue.front()->operation == 2 && changes == 2);
  queue.answerFront(false);
  TEST_CHECK(!answers[1].approved && queue.size() == 0);
  queue.answerFront(true);  // nothing pending: no effect
  TEST_CHECK(answers.size() == 2);

  // One client cannot crowd out the user.
  for (std::uint64_t i = 10; i < 16; ++i) queue.enqueue(action(i));
  TEST_CHECK(queue.size() == ConfirmationQueue::kMaxPerClient);
  const auto refused = std::count_if(answers.begin(), answers.end(), [](const Answer& a) { return a.operation >= 10 && !a.approved; });
  TEST_CHECK(refused == 3);
  for (std::uint64_t i = 20; i < 30; ++i) queue.enqueue(action(i, "exe:/c" + std::to_string(i)));
  TEST_CHECK(queue.size() == ConfirmationQueue::kMaxPending);  // total cap, extras refused immediately

  answers.clear();
  queue.denyAll();
  TEST_CHECK(queue.size() == 0 && answers.size() == ConfirmationQueue::kMaxPending);
  TEST_CHECK(std::all_of(answers.begin(), answers.end(), [](const Answer& a) { return !a.approved; }));

  // The service can settle an operation first (timeout, requester left).
  queue.enqueue(action(40));
  queue.enqueue(action(41, "exe:/other"));
  queue.dropOperation(40);
  TEST_CHECK(queue.size() == 1 && queue.front()->operation == 41);
  queue.dropOperation(999);
  TEST_CHECK(queue.size() == 1);
}

void checkGrantNarrowing() {
  std::vector<Answer> answers;
  ConfirmationQueue queue([&](std::uint64_t operation, bool approved, const std::vector<std::string>& tiers) {
    answers.push_back({operation, approved, tiers});
  });
  ConfirmRequest grant;
  grant.operation = 5;
  grant.kind = ConfirmRequest::Kind::Grant;
  grant.client = "exe:/x";
  grant.tiers = {"observe", "act"};

  queue.enqueue(grant);
  queue.answerFront(true, {"observe", "confirm", "blocked"});  // tiers the user never saw are ignored
  TEST_CHECK(answers[0].approved && answers[0].tiers == std::vector<std::string>{"observe"});

  grant.operation = 6;
  queue.enqueue(grant);
  queue.answerFront(true, {"confirm"});  // nothing left of what was asked: that is a denial
  TEST_CHECK(!answers[1].approved && answers[1].tiers.empty());

  grant.operation = 7;
  queue.enqueue(grant);
  queue.answerFront(false, {"observe"});
  TEST_CHECK(!answers[2].approved);
}

}  // namespace

int main() {
  checkSanitize();
  checkFriendlyName();
  checkParse();
  checkClientName();
  checkQueue();
  checkGrantNarrowing();
  return 0;
}
