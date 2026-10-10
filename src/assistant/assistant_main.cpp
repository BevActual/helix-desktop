// helix-assistant "turn the volume up": plain English in, one desktop action out.
//
// The model (helix-inference) only chooses a tool call, constrained to the tools this client is allowed to use. Anything that
// changes the desktop is shown first and runs only after a yes typed here. The call then goes through Agent Surface like any other
// client's, so its grants, confirmations and audit apply on top. Design: docs/design/agent-and-inference.md.
#include "assistant/assistant_core.h"
#include "cli/schema_msg.h"
#include "desktop/shell_tools.h"
#include "desktop/tool_schema.h"
#include "desktop_api/protocol.h"
#include "inference/protocol.h"
#include "inference/routing.h"

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
  const fs::path installed = fs::path(HELIX_INSTALL_PREFIX) / HELIX_INSTALL_DATADIR / "helix" / name;
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

// Where the answer goes. Text mode talks to a person at a terminal. JSON mode (--json) is for the shell's assistant panel: one
// JSON object on stdout per invocation, so the panel needs no terminal and the shell never holds the model's authority.
struct Output {
  bool json = false;
  void say(const std::string& line) const { if (!json) std::println("{}", line); }
  void complain(const std::string& line) const { if (!json) std::println(stderr, "{}", line); }
  // kind: done | confirm | explain | error
  void result(const std::string& kind, Json fields) const {
    if (!json) return;
    fields["kind"] = kind;
    std::println("{}", fields.dump());
  }
};

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
  bool dryRun = false, warm = false, execute = false;
  Output out;
  std::string request, requestText;
  std::vector<std::string> executeArgs;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (execute && !executeArgs.empty()) { executeArgs.push_back(arg); continue; }
    if (arg == "--dry-run") dryRun = true;
    else if (arg == "--warm") warm = true;
    else if (arg == "--json") out.json = true;
    else if (arg == "--request" && i + 1 < argc) requestText = argv[++i];
    else if (arg == "--execute") { execute = true; if (i + 1 < argc) executeArgs.push_back(argv[++i]); }  // then: tool [arguments...]
    else request += (request.empty() ? "" : " ") + arg;
  }
  const auto fail = [&](const std::string& text, int code = 1) {
    out.complain(text);
    out.result("error", {{"text", text}});
    return code;
  };
  if (warm) request = "Turn the volume up.";  // any request: the prompt prefix is what gets cached
  if (request.empty() && !execute) {
    std::println(stderr, "usage: helix-assistant [--dry-run] [--json] <what you want done>\n       helix-assistant --warm    load the model and prepare its memory, so the first request is fast\n       helix-assistant --json --request <text> --execute <tool> [arguments...]    run a call that was shown and approved");
    return 2;
  }
  if (request.size() > 500) return fail("That request is too long (500 characters at most).", 2);

  auto policy = helix::agent::RiskPolicy::load(dataFile("HELIX_RISK_POLICY", "msg-risk.toml"));
  if (!policy) return fail(policy.error());
  auto hints = helix::agent::ToolHints::load(dataFile("HELIX_TOOL_HINTS", "tool-hints.toml"));
  if (!hints) return fail(hints.error());

  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) return fail(std::string("No session bus: ") + (error ? error->message : "?"));

  try {
    using helix::agent::Tier;
    const char* desktop = helix::busName; const char* desktopPath = helix::objectPath; const char* agentIface = helix::agentSurfaceInterface;
    Waiter agentDone(bus, desktop, desktopPath, agentIface);
    Waiter chatDone(bus, helix::inference::busName, helix::inference::objectPath, helix::inference::interfaceName);

    const Json session = call(bus, desktop, desktopPath, agentIface, "Hello", g_variant_new("(ss)", "Helix assistant", "1"))[0];
    auto grantedTiers = [&](const Json& tiers) {
      std::set<Tier> result;
      for (const auto& name : tiers) if (auto tier = helix::agent::parseTier(name.get<std::string>())) result.insert(*tier);
      return result;
    };
    std::set<Tier> granted = grantedTiers(session.at("tiers"));
    if (!granted.contains(Tier::Observe) || !granted.contains(Tier::Act)) {
      if (!session.value("confirmer", false)) return fail("The assistant has no permission yet, and the desktop shell is not running to ask you.");
      out.say("The assistant needs your permission to read the desktop state and make everyday changes. Look for the approval dialog.");
      const auto operation = call(bus, desktop, desktopPath, agentIface, "RequestAccess", g_variant_new_parsed("(['observe', 'act'],)"))[0].get<guint64>();
      const auto answer = agentDone.wait(operation, 90);
      if (!answer.ok) return fail("No permission was given: " + answer.result.value("error", "denied"));
      granted = grantedTiers(answer.result.at("granted"));
    }

    // Runs a call through Agent Surface and reports the outcome. The service decides; this only carries the result back.
    const auto run = [&](const Reply& reply, const std::string& why) {
      Json action{{"argv", Json::array()}, {"literal", reply.args}, {"reason", why.substr(0, 200)}};
      const auto operation = call(bus, desktop, desktopPath, agentIface, "Action", g_variant_new("(s@a{sv})", reply.tool.c_str(), helix::toVariant(action)))[0].get<guint64>();
      const auto result = agentDone.wait(operation, 90);  // a confirm-tier call can wait for the person
      if (!result.ok) return fail("Not done: " + result.result.value("error", "failed"));
      const std::string content = result.result.value("content", "");
      const bool untrusted = result.result.value("trust", "") == "untrusted";
      out.say((untrusted ? "(from outside sources, not verified)\n" : "") + (content.empty() ? std::string("Done.") : content));
      out.result("done", {{"text", content.empty() ? "Done." : content}, {"untrusted", untrusted}, {"description", describe(reply, &*hints)}});
      return 0;
    };

    if (execute) {
      // A call the person has just been shown and approved. It gets the same checks as a model's reply: the tool must exist and
      // be within this client's grants, and the service still applies its own.
      Reply reply;
      reply.kind = Reply::Kind::Call;
      reply.tool = executeArgs.empty() ? "" : executeArgs.front();
      reply.args.assign(executeArgs.begin() + (executeArgs.empty() ? 0 : 1), executeArgs.end());
      const std::set<std::string> shellTools = helix::agent::shellSupportedTools();
      if (stepFor(*policy, granted, reply) == Step::Refuse || reply.tool == helix::agent::kDeclineTool ||
          (!shellTools.empty() && !shellTools.contains(reply.tool))) {
        out.say("That is not something this assistant is allowed to do.");
        out.result("explain", {{"text", "That is not something this assistant is allowed to do."}});
        return 3;
      }
      return run(reply, requestText.empty() ? "approved in the assistant panel" : requestText);
    }

    // Offer the model only what this client may do, and only the tiers the evaluation covered.
    std::set<Tier> offered;
    for (const Tier tier : {Tier::Observe, Tier::ObservePrivate, Tier::Act}) if (granted.contains(tier)) offered.insert(tier);
    if (offered.empty()) return fail("The assistant has no usable permission.");
    const auto& root = helix::cli::kMsgCmd;
    // The model is shown every tool: it is best at finding the right one (96%), and a request for something this desktop cannot do then
    // comes back as that tool, which is turned into a decline below. Offering only the tools the shell has made it pick a similar-sounding
    // one instead (held-out: 27 of 43 requests it should have declined, against 1 of 43 this way). tests/inference_eval.cpp --map-unsupported.
    const std::set<std::string> carriedOut = helix::agent::shellSupportedTools();
    const Json schema = helix::agent::toolCallSchema(root, *policy, offered, nullptr, true);
    const std::string catalog = helix::agent::toolCatalog(root, *policy, offered, nullptr, true, &*hints);

    // Act slot: task=act selects the on-device grammar model from inference-routing.toml when model is omitted.
    // HELIX_ASSISTANT_MODEL still wins when set and installed.
    std::string preferred = std::getenv("HELIX_ASSISTANT_MODEL") ? std::getenv("HELIX_ASSISTANT_MODEL") : "";
    if (preferred.empty()) preferred = helix::inference::loadRouting().route("act").model;
    const auto chosen = chooseModel(call(bus, helix::inference::busName, helix::inference::objectPath, helix::inference::interfaceName, "ListModels", nullptr)[0],
                                    preferred);
    if (!chosen.error.empty()) return fail(chosen.error);
    Json options{{"messages", buildMessages(catalog, request)},
                 {"schema", schema.dump()},
                 {"task", "act"},
                 {"model", chosen.id},
                 {"max_tokens", warm ? 1 : 128},
                 {"temperature", 0.0},
                 {"priority", warm ? "background" : "interactive"}};
    const auto chat = call(bus, helix::inference::busName, helix::inference::objectPath, helix::inference::interfaceName, "Action",
                           g_variant_new("(s@a{sv})", "chat", helix::toVariant(options)))[0].get<guint64>();
    const auto answer = chatDone.wait(chat, 300);
    if (warm) {
      if (!answer.ok && !answer.result.value("truncated", false)) return fail("Could not warm the model: " + answer.result.value("error", "unknown error"));
      out.say("Model " + chosen.id + " is ready.");
      out.result("done", {{"text", "Model " + chosen.id + " is ready."}});
      return 0;
    }
    if (!answer.ok) return fail("The model could not answer: " + answer.result.value("error", "unknown error"));

    Reply reply = parseReply(answer.result.value("text", ""));
    if (reply.kind == Reply::Kind::Call && !carriedOut.empty() && !carriedOut.contains(reply.tool)) {
      // A real tool, but not one this desktop's shell can carry out: say so, and do nothing.
      reply.kind = Reply::Kind::Decline;
      reply.args.clear();
      reply.reason = "this desktop cannot do that yet (" + describe(reply, &*hints) + ")";
    }
    switch (stepFor(*policy, granted, reply)) {
      case Step::Explain: {
        const std::string text = reply.kind == Reply::Kind::Decline ? "I can't do that: " + reply.reason : "I couldn't work out what to do: " + reply.error;
        out.say(text);
        out.result("explain", {{"text", text}});
        return 3;
      }
      case Step::Refuse: {
        const std::string text = "I can't do that: " + reply.tool + " is not something this assistant is allowed to use.";
        out.say(text);
        out.result("explain", {{"text", text}});
        return 3;
      }
      case Step::AskFirst:
        out.say("About to: " + describe(reply, &*hints));
        if (!reply.reason.empty()) out.say("Because: " + reply.reason);
        if (out.json) {  // the panel shows this, and comes back with --execute if the person agrees
          out.result("confirm", {{"description", describe(reply, &*hints)}, {"reason", reply.reason}, {"tool", reply.tool}, {"args", reply.args}});
          return 0;
        }
        if (dryRun) { out.say("(dry run: nothing was done)"); return 0; }
        if (!askYesNo("Go ahead?")) { out.say("Cancelled. Nothing was changed."); return 0; }
        break;
      case Step::Run:
        out.say("Checking: " + describe(reply, &*hints));
        if (dryRun) { out.say("(dry run: nothing was done)"); return 0; }
        break;
    }
    return run(reply, request);
  } catch (const std::exception& failure) {
    return fail(failure.what());
  }
}
