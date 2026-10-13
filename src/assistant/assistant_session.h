#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The state of one conversation with the assistant, with no UI and no process handling in it. The panel shows it; a
// runner starts `helix-assistant` and hands back its output. Keeping it separate means the rules that matter are
// testable:
//   - a change is only ever run after the person confirms it, and only the exact call that was shown;
//   - cancelling or asking again makes any late answer from an earlier request harmless;
//   - everything shown is display-safe, because it comes from a model or from desktop state.
namespace helix::assistant_ui {

  enum class Phase {
    Idle,     // waiting for a request
    Thinking, // the model is choosing
    Confirm,  // a change is waiting for a yes
    Acting,   // an approved call is running
    Result,   // an answer, a refusal or an error is on screen
  };

  struct View {
    Phase phase = Phase::Idle;
    std::string text;        // Result: what to show
    std::string description; // Confirm: what is about to happen, in plain words
    std::string reason;      // Confirm: the model's own explanation, if it gave one
    bool failed = false;     // Result: an error or a refusal, not an answer
    bool unverified = false; // Result: the answer comes from outside sources (clipboard, notifications)
  };

  class Session {
  public:
    // Starts helix-assistant with `arguments`; later calls `done(exitCode, standardOutput)` on the thread that owns the
    // Session. Returns false if it could not be started.
    using Done = std::function<void(int exitCode, std::string output)>;
    using Runner = std::function<bool(std::vector<std::string> arguments, Done done)>;

    Session(Runner runner, std::function<void()> changed) : runner_(std::move(runner)), changed_(std::move(changed)) {}

    static constexpr std::size_t kMaxRequestChars = 500;

    void submit(const std::string& request); // ignored unless Idle or Result
    void confirm();                          // ignored unless Confirm
    void cancel();                           // back to Idle from anywhere; any answer still on its way is dropped
    [[nodiscard]] const View& view() const { return view_; }

  private:
    void set(View view);
    void fail(const std::string& text);
    void handlePlan(int exitCode, const std::string& output, std::uint64_t generation);
    void handleResult(const std::string& output, std::uint64_t generation);

    Runner runner_;
    std::function<void()> changed_;
    View view_;
    std::uint64_t generation_ = 0; // bumped by every submit and cancel; answers from older ones are ignored
    std::string request_;
    std::string tool_;
    std::vector<std::string> args_;
  };

} // namespace helix::assistant_ui
