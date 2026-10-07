// helix-assistant "turn the volume up": plain English in, one desktop action out.
//
// The model (helix-inference) only chooses a tool call, constrained to the tools this client is allowed to use. Anything that
// changes the desktop is shown first and runs only after a yes typed here. The call then goes through Agent1 like any other
// client's, so its grants, confirmations and audit apply on top. Design: docs/design/agent-and-inference.md.
#include "assistant/assistant_core.h"
#include "cli/schema_msg.h"
#include "desktop/tool_schema.h"
#include "desktop_api/protocol.h"
#include "inference/protocol.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <print>
#include <string>
#include <unistd.h>

using helix::Json;
namespace fs = std::filesystem;

namespace {

fs::path dataFile(const char* env, const char* name) {
  if (const char* value = std::getenv(env); value && *value) return value;
  std::error_code ec;
  const fs::path installed = fs::path(NOCTALIA_INSTALL_PREFIX) / NOCTALIA_INSTALL_DATADIR / "helix" / name;
  if (fs::exists(installed, ec)) return installed;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  return exe.parent_path() / ".." / "protocols" / "ipc" / name;  // development builds run from <repo>/<builddir>/
}

// Completed signals of one service, collected as they arrive so a fast reply is never missed.
class Waiter {
public:
  Waiter(GDBusConnection* bus, const char* name, const char* path, const char* iface) {
    id_ = g_dbus_connection_signal_subscribe(bus, name, iface, "Completed", path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onCompleted, this, nullptr);
    bus_ = bus;
  }
  ~Waiter() { g_dbus_connection_signal_unsubscribe(bus_, id_); }
  struct Done { bool ok = false; Json result; };
  Done wait(guint64 operation, int seconds) {
    const gint64 end = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;
    while (!done_.contains(operation)) {
      if (g_get_monotonic_time() > end) return {false, {{"error", "timed out"}}};
      g_main_context_iteration(nullptr, FALSE);
      g_usleep(2000);
    }
    return done_.at(operation);
  }

private:
  static void onCompleted(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer self) {
    guint64 operation = 0; gboolean ok = FALSE; const gchar* text = "";
    g_variant_get(params, "(tbs)", &operation, &ok, &text);
    auto parsed = Json::parse(text, nullptr, false);
    static_cast<Waiter*>(self)->done_[operation] = {ok != FALSE, parsed.is_discarded() ? Json{{"error", text}} : parsed};
  }
  GDBusConnection* bus_ = nullptr;
  guint id_ = 0;
  std::map<guint64, Done> done_;
};

Json call(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method, GVariant* params) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(bus, name, path, iface, method, params, nullptr, G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &error);
  if (!reply) {
    const std::string message = error ? error->message : "call failed";
    g_clear_error(&error);
    throw std::runtime_error(message);
  }
  Json out = helix::fromVariant(reply);
  g_variant_unref(reply);
  return out;
}

bool askYesNo(const std::string& question) {
  if (!isatty(STDIN_FILENO) && !std::getenv("HELIX_ASSISTANT_ANSWER_FROM_STDIN")) {
    std::println(stderr, "Not asking: no terminal to answer on. Nothing was changed.");
    return false;
  }
  std::print("{} [y/N] ", question);
  std::fflush(stdout);
  std::string line;
  std::getline(std::cin, line);
  return line == "y" || line == "Y" || line == "yes";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace helix::assistant;
  bool dryRun = false, warm = false;
  std::string request;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dry-run") dryRun = true;
    else if (arg == "--warm") warm = true;
    else request += (request.empty() ? "" : " ") + arg;
  }
  if (warm) request = "Turn the volume up.";  // any request: the prompt prefix is what gets cached
  if (request.empty()) { std::println(stderr, "usage: helix-assistant [--dry-run] <what you want done>\n       helix-assistant --warm    load the model and prepare its memory, so the first request is fast"); return 2; }
  if (request.size() > 500) { std::println(stderr, "That request is too long (500 characters at most)."); return 2; }

  auto policy = helix::agent::RiskPolicy::load(dataFile("HELIX_RISK_POLICY", "msg-risk.toml"));
  if (!policy) { std::println(stderr, "{}", policy.error()); return 1; }
  auto hints = helix::agent::ToolHints::load(dataFile("HELIX_TOOL_HINTS", "tool-hints.toml"));
  if (!hints) { std::println(stderr, "{}", hints.error()); return 1; }

  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) { std::println(stderr, "No session bus: {}", error ? error->message : "?"); return 1; }

  try {
    using helix::agent::Tier;
    const char* desktop = helix::busName; const char* desktopPath = helix::objectPath; const char* agentIface = helix::agentInterface;
    Waiter agentDone(bus, desktop, desktopPath, agentIface);
    Waiter chatDone(bus, helix::inference::busName, helix::inference::objectPath, helix::inference::interfaceName);

    const Json session = call(bus, desktop, desktopPath, agentIface, "Hello", g_variant_new("(ss)", "Helix assistant", "1"))[0];
    auto grantedTiers = [&](const Json& tiers) {
      std::set<Tier> out;
      for (const auto& name : tiers) if (auto tier = helix::agent::parseTier(name.get<std::string>())) out.insert(*tier);
      return out;
    };
    std::set<Tier> granted = grantedTiers(session.at("tiers"));
    if (!granted.contains(Tier::Observe) || !granted.contains(Tier::Act)) {
      if (!session.value("confirmer", false)) {
        std::println(stderr, "The assistant has no permission yet, and the desktop shell is not running to ask you.");
        return 1;
      }
      std::println("The assistant needs your permission to read the desktop state and make everyday changes. Look for the approval dialog.");
      const auto operation = call(bus, desktop, desktopPath, agentIface, "RequestAccess", g_variant_new_parsed("(['observe', 'act'],)"))[0].get<guint64>();
      const auto answer = agentDone.wait(operation, 90);
      if (!answer.ok) { std::println(stderr, "No permission was given: {}", answer.result.value("error", "denied")); return 1; }
      granted = grantedTiers(answer.result.at("granted"));
    }

    // Offer the model only what this client may do, and only the tiers the evaluation covered.
    std::set<Tier> offered;
    for (const Tier tier : {Tier::Observe, Tier::ObservePrivate, Tier::Act}) if (granted.contains(tier)) offered.insert(tier);
    if (offered.empty()) { std::println(stderr, "The assistant has no usable permission."); return 1; }
    const auto& root = noctalia::cli::kMsgCmd;
    const Json schema = helix::agent::toolCallSchema(root, *policy, offered, nullptr, true);
    const std::string catalog = helix::agent::toolCatalog(root, *policy, offered, nullptr, true, &*hints);

    const auto chosen = chooseModel(call(bus, helix::inference::busName, helix::inference::objectPath, helix::inference::interfaceName, "ListModels", nullptr)[0],
                                    std::getenv("HELIX_ASSISTANT_MODEL") ? std::getenv("HELIX_ASSISTANT_MODEL") : "");
    if (!chosen.error.empty()) { std::println(stderr, "{}", chosen.error); return 1; }
    Json options{{"messages", buildMessages(catalog, request)}, {"schema", schema.dump()}, {"model", chosen.id}, {"max_tokens", warm ? 1 : 128},
                 {"temperature", 0.0}, {"priority", warm ? "background" : "interactive"}};
    const auto chat = call(bus, helix::inference::busName, helix::inference::objectPath, helix::inference::interfaceName, "Action",
                           g_variant_new("(s@a{sv})", "chat", helix::toVariant(options)))[0].get<guint64>();
    const auto answer = chatDone.wait(chat, 300);
    if (warm) {
      if (!answer.ok && !answer.result.value("truncated", false)) { std::println(stderr, "Could not warm the model: {}", answer.result.value("error", "unknown error")); return 1; }
      std::println("Model {} is ready.", chosen.id);
      return 0;
    }
    if (!answer.ok) { std::println(stderr, "The model could not answer: {}", answer.result.value("error", "unknown error")); return 1; }

    const Reply reply = parseReply(answer.result.value("text", ""));
    switch (stepFor(*policy, granted, reply)) {
      case Step::Explain:
        std::println("{}", reply.kind == Reply::Kind::Decline ? "I can't do that: " + reply.reason : "I couldn't work out what to do: " + reply.error);
        return 3;
      case Step::Refuse:
        std::println("I can't do that: {} is not something this assistant is allowed to use.", reply.tool);
        return 3;
      case Step::AskFirst:
        std::println("About to: {}", describe(reply, &*hints));
        if (!reply.reason.empty()) std::println("Because: {}", reply.reason);
        if (dryRun) { std::println("(dry run: nothing was done)"); return 0; }
        if (!askYesNo("Go ahead?")) { std::println("Cancelled. Nothing was changed."); return 0; }
        break;
      case Step::Run:
        std::println("Checking: {}", describe(reply, &*hints));
        if (dryRun) { std::println("(dry run: nothing was done)"); return 0; }
        break;
    }

    Json action{{"argv", Json::array()}, {"literal", reply.args}, {"reason", request.substr(0, 200)}};
    const auto operation = call(bus, desktop, desktopPath, agentIface, "Action", g_variant_new("(s@a{sv})", reply.tool.c_str(), helix::toVariant(action)))[0].get<guint64>();
    const auto result = agentDone.wait(operation, 90);  // a confirm-tier call can wait for the person
    if (!result.ok) { std::println(stderr, "Not done: {}", result.result.value("error", "failed")); return 1; }
    const std::string content = result.result.value("content", "");
    std::println("{}{}", result.result.value("trust", "") == "untrusted" ? "(from outside sources, not verified)\n" : "", content.empty() ? "Done." : content);
    return 0;
  } catch (const std::exception& failure) {
    std::println(stderr, "{}", failure.what());
    return 1;
  }
}
