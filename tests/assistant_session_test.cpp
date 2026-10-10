#include "assistant/assistant_session.h"
#include "tests/test_check.h"

#include <deque>

using namespace helix::assistant_ui;

namespace {

// A runner that records what was started and lets the test decide when and how each run finishes.
struct Harness {
  std::vector<std::vector<std::string>> started;
  std::deque<Session::Done> pending;
  bool canStart = true;
  int changes = 0;
  Session session{[this](std::vector<std::string> arguments, Session::Done done) {
                    if (!canStart) return false;
                    started.push_back(std::move(arguments));
                    pending.push_back(std::move(done));
                    return true;
                  },
                  [this] { ++changes; }};
  void finish(const std::string& output, int code = 0) {
    auto done = std::move(pending.front());
    pending.pop_front();
    done(code, output);
  }
};

const char* kConfirm = R"j({"kind":"confirm","description":"Make the screen dimmer (`brightness-down`)","reason":"you asked","tool":"brightness-down","args":[]})j";

}  // namespace

int main() {
  // A request goes out as JSON mode; nothing runs until the person confirms.
  {
    Harness h;
    h.session.submit("  make the screen dimmer \n");
    TEST_CHECK(h.session.view().phase == Phase::Thinking);
    TEST_CHECK(h.started.size() == 1 && h.started[0] == std::vector<std::string>({"--json", "make the screen dimmer"}));
    h.finish(kConfirm);
    TEST_CHECK(h.session.view().phase == Phase::Confirm && h.started.size() == 1);
    TEST_CHECK(h.session.view().description == "Make the screen dimmer (`brightness-down`)" && h.session.view().reason == "you asked");

    // Yes runs exactly the call that was shown, with the request as the stated reason.
    h.session.confirm();
    TEST_CHECK(h.session.view().phase == Phase::Acting && h.started.size() == 2);
    TEST_CHECK(h.started[1] == std::vector<std::string>({"--json", "--request", "make the screen dimmer", "--execute", "brightness-down"}));
    h.session.confirm();  // a second yes cannot run it again
    TEST_CHECK(h.started.size() == 2);
    h.finish(R"j({"kind":"done","text":"Done."})j");
    TEST_CHECK(h.session.view().phase == Phase::Result && !h.session.view().failed && h.session.view().text == "Done.");
  }

  // Arguments are passed through unchanged, after the tool.
  {
    Harness h;
    h.session.submit("dark mode");
    h.finish(R"j({"kind":"confirm","description":"Switch to dark","reason":"","tool":"theme-mode-set","args":["dark"]})j");
    h.session.confirm();
    TEST_CHECK(h.started[1].back() == "dark" && h.started[1][4] == "theme-mode-set");
  }

  // Reading is answered at once and nothing is asked.
  {
    Harness h;
    h.session.submit("is the bar visible?");
    h.finish(R"j({"kind":"done","text":"{\"barVisible\":true}","untrusted":true})j");
    TEST_CHECK(h.session.view().phase == Phase::Result && h.session.view().unverified && h.started.size() == 1);
  }

  // Declines and errors are shown as failures and never run anything.
  {
    Harness h;
    h.session.submit("restart");
    h.finish(R"j({"kind":"explain","text":"I can't do that: no tool for it."})j", 3);
    TEST_CHECK(h.session.view().failed && h.session.view().text == "I can't do that: no tool for it." && h.started.size() == 1);
    h.session.confirm();  // nothing to confirm
    TEST_CHECK(h.started.size() == 1);
    h.session.submit("again");  // a new request can follow a result
    h.finish(R"j({"kind":"error","text":"The model could not answer"})j", 1);
    TEST_CHECK(h.session.view().failed);
  }

  // Cancelling makes a late answer harmless, so a request cannot come back to life after the person walked away.
  {
    Harness h;
    h.session.submit("dim");
    h.session.cancel();
    TEST_CHECK(h.session.view().phase == Phase::Idle);
    h.finish(kConfirm);
    TEST_CHECK(h.session.view().phase == Phase::Idle);
    h.session.submit("dim");
    h.finish(kConfirm);
    h.session.cancel();  // cancelling at the confirmation drops the call
    h.session.confirm();
    TEST_CHECK(h.started.size() == 2 && h.session.view().phase == Phase::Idle);
  }

  // A stale answer cannot overwrite a newer request or be confirmed as it.
  {
    Harness h;
    h.session.submit("first");
    h.session.cancel();
    h.session.submit("second");
    h.finish(kConfirm);  // the first one finishing late (queued first)
    TEST_CHECK(h.session.view().phase == Phase::Thinking);
    h.finish(R"j({"kind":"done","text":"second answer"})j");
    TEST_CHECK(h.session.view().text == "second answer");
  }

  // Cancelling while a change is running does not undo it, but the late result is dropped rather than shown as a new request's.
  {
    Harness h;
    h.session.submit("dim");
    h.finish(kConfirm);
    h.session.confirm();
    h.session.cancel();
    h.finish(R"j({"kind":"done","text":"Done."})j");
    TEST_CHECK(h.session.view().phase == Phase::Idle);
  }

  // Requests while busy are ignored; empty and oversized ones never start anything.
  {
    Harness h;
    h.session.submit("one");
    h.session.submit("two");
    TEST_CHECK(h.started.size() == 1);
    h.finish(kConfirm);
    h.session.submit("three");  // a change is waiting for an answer
    TEST_CHECK(h.started.size() == 1 && h.session.view().phase == Phase::Confirm);
    h.session.cancel();
    h.session.submit("   ");
    TEST_CHECK(h.started.size() == 1 && h.session.view().phase == Phase::Idle);
    h.session.submit(std::string(501, 'x'));
    TEST_CHECK(h.started.size() == 1 && h.session.view().failed);
  }

  // Output that is not what the assistant promises is an error, never a call to run.
  for (const char* output : {"", "not json", "[]", R"j({"kind":5})j", R"j({"kind":"confirm"})j",
                             R"j({"kind":"confirm","description":"x","tool":"","args":[]})j",
                             R"j({"kind":"confirm","description":"x","tool":"a","args":[1]})j",
                             R"j({"kind":"confirm","description":"","tool":"a","args":[]})j"}) {
    Harness h;
    h.session.submit("x");
    h.finish(output);
    TEST_CHECK(h.session.view().phase == Phase::Result && h.session.view().failed);
    h.session.confirm();
    TEST_CHECK(h.started.size() == 1);
  }

  // Text from the model is made display-safe: no control characters, no faked extra lines, bounded.
  {
    Harness h;
    h.session.submit("x");
    h.finish(R"j({"kind":"confirm","description":"Line one\nLine two\u0007\u202e","reason":"","tool":"a","args":[]})j");
    TEST_CHECK(h.session.view().description.find('\n') == std::string::npos && h.session.view().description.find('\a') == std::string::npos);
    TEST_CHECK(h.session.view().description.find("\xE2\x80\xAE") == std::string::npos);
  }

  // If the assistant is not installed the person is told, and the session is usable again.
  {
    Harness h;
    h.canStart = false;
    h.session.submit("x");
    TEST_CHECK(h.session.view().phase == Phase::Result && h.session.view().failed);
    h.canStart = true;
    h.session.submit("x");
    TEST_CHECK(h.session.view().phase == Phase::Thinking);
  }
  return 0;
}
