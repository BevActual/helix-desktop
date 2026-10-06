#include "desktop/agent_endpoint.h"

#include "desktop/protocol.h"
#include "ipc/ipc_client.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace helix::agent {

namespace {

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Invalid : std::invalid_argument { using std::invalid_argument::invalid_argument; };

std::filesystem::path locatePolicy() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HELIX_RISK_POLICY"); env && *env) return env;
  const fs::path installed = fs::path(NOCTALIA_INSTALL_PREFIX) / NOCTALIA_INSTALL_DATADIR / "helix" / "msg-risk.toml";
  std::error_code ec;
  if (fs::exists(installed, ec)) return installed;
  // Development builds run from <repo>/<builddir>/.
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  return exe.parent_path() / ".." / "protocols" / "ipc" / "msg-risk.toml";
}

// The shell socket is named after WAYLAND_DISPLAY, which an activated service may not inherit.
std::string shellSocket() {
  namespace fs = std::filesystem;
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || !*runtime) return {};
  const char* display = std::getenv("WAYLAND_DISPLAY");
  std::error_code ec;
  if (display && *display && fs::exists(fs::path(runtime) / (std::string("noctalia-") + display + ".sock"), ec)) return {};
  fs::path newest;
  fs::file_time_type newestTime{};
  for (const auto& entry : fs::directory_iterator(runtime, ec)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with("noctalia-wayland") || !name.ends_with(".sock")) continue;
    const auto time = entry.last_write_time(ec);
    if (newest.empty() || time > newestTime) { newest = entry.path(); newestTime = time; }
  }
  return newest.string();
}

std::set<Tier> tiersFrom(const Json& value, bool allowBlockedName = false) {
  if (!value.is_array()) throw Invalid("tiers must be an array of strings");
  std::set<Tier> tiers;
  for (const auto& item : value) {
    const auto tier = item.is_string() ? parseTier(item.get<std::string>()) : std::nullopt;
    if (!tier || (*tier == Tier::Blocked && !allowBlockedName)) throw Invalid("unknown or ungrantable tier");
    tiers.insert(*tier);
  }
  return tiers;
}

std::vector<std::string> stringsFrom(const Json& object, const char* key) {
  std::vector<std::string> out;
  if (!object.contains(key)) return out;
  if (!object.at(key).is_array()) throw Invalid(std::string(key) + " must be an array of strings");
  for (const auto& item : object.at(key)) {
    if (!item.is_string()) throw Invalid(std::string(key) + " must be an array of strings");
    out.push_back(item.get<std::string>());
  }
  return out;
}

Json tiersJson(const std::set<Tier>& tiers) {
  Json out = Json::array();
  for (const Tier tier : tiers) out.push_back(std::string(tierName(tier)));
  return out;
}

struct Deferred {
  std::function<void()> run;
};

void defer(std::function<void()> run) {
  g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
    std::unique_ptr<Deferred> task(static_cast<Deferred*>(data));
    task->run();
    return G_SOURCE_REMOVE;
  }, new Deferred{std::move(run)}, nullptr);
}

// Runs the command on a worker thread so a slow shell action cannot stall the service's main loop.
void executeInShell(const std::string& tool, const std::vector<std::string>& argv, const std::vector<std::string>& literal,
                    std::function<void(bool, std::string)> done) {
  auto line = buildWireCommand(tool, argv, literal);
  if (!line) {
    defer([done = std::move(done), reason = line.error()] { done(false, reason); });
    return;
  }
  std::thread([line = std::move(*line), done = std::move(done), socket = shellSocket()]() mutable {
    std::string response, error;
    const bool reached = IpcClient::exchange(line, response, error, socket);
    while (!response.empty() && response.back() == '\n') response.pop_back();
    const bool ok = reached && !response.starts_with("error:");
    std::string text = reached ? std::move(response) : std::move(error);
    defer([done = std::move(done), ok, text = std::move(text)] { done(ok, text); });
  }).detach();
}

}  // namespace

AgentEndpoint::AgentEndpoint(GDBusConnection* bus, std::string objectPath, std::string interfaceName,
                             std::function<std::string()> desktopOwner)
    : bus_(bus), objectPath_(std::move(objectPath)), interface_(std::move(interfaceName)),
      desktopOwner_(std::move(desktopOwner)) {
  const auto policyFile = locatePolicy();
  auto policy = RiskPolicy::load(policyFile);
  if (!policy) {
    unavailable_ = policy.error() + " (" + policyFile.string() + ")";
    return;
  }
  const std::filesystem::path stateDir = std::filesystem::path(FileUtils::stateDir()) / "agent";
  std::error_code ec;
  FileUtils::createPrivateDirectories(stateDir, ec);
  grants_ = std::make_unique<GrantStore>(stateDir / "grants.json");
  if (auto loaded = grants_->load(); !loaded) g_warning("Agent grants ignored: %s", loaded.error().c_str());
  auditFile_ = stateDir / "audit.jsonl";
  broker_ = std::make_unique<AgentBroker>(std::move(*policy), *grants_, AuditLog(auditFile_), executeInShell,
                                          [] { return std::chrono::steady_clock::now(); });
  broker_->onConfirmationRequested = [this](std::uint64_t, const Json& details) {
    if (!confirmer_.empty()) emit("ConfirmationRequested", g_variant_new("(@a{sv})", toVariant(details)), confirmer_);
  };
}

void AgentEndpoint::emit(const char* signal, GVariant* value, const std::string& destination) {
  g_dbus_connection_emit_signal(bus_, destination.empty() ? nullptr : destination.c_str(), objectPath_.c_str(),
                                interface_.c_str(), signal, value, nullptr);
}

const AgentEndpoint::Peer& AgentEndpoint::peer(const std::string& sender) {
  if (const auto found = peers_.find(sender); found != peers_.end()) return found->second;
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "GetConnectionCredentials", g_variant_new("(s)", sender.c_str()), G_VARIANT_TYPE("(a{sv})"),
    G_DBUS_CALL_FLAGS_NONE, 500, nullptr, &error);
  if (!reply) {
    const std::string message = error ? error->message : "unknown error";
    if (error) g_error_free(error);
    throw Failure("could not identify the caller: " + message);
  }
  GVariant* dict = g_variant_get_child_value(reply, 0);
  guint32 pid = 0, uid = 0;
  const bool havePid = g_variant_lookup(dict, "ProcessID", "u", &pid);
  const bool haveUid = g_variant_lookup(dict, "UnixUserID", "u", &uid);
  g_variant_unref(dict);
  g_variant_unref(reply);
  if (!havePid || !haveUid) throw Failure("the bus did not report the caller's process");
  if (uid != geteuid()) throw Invalid("callers must run as the same user");

  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
  if (ec) throw Failure("could not read the caller's executable");
  std::vector<std::string> cmdline;
  if (std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary); in) {
    for (std::string part; std::getline(in, part, '\0');) cmdline.push_back(part);
  }
  Peer who{clientIdentity(exe.string(), cmdline), exe.string(), {}};
  return peers_.emplace(sender, std::move(who)).first->second;
}

void AgentEndpoint::complete(const std::string& sender, std::shared_ptr<std::uint64_t> operation, bool ok, std::string result) {
  // Deferred so the method reply carrying the operation id always precedes its Completed signal.
  defer([this, sender, operation, ok, result = std::move(result)] {
    operations_.erase(*operation);
    emit("Completed", g_variant_new("(tbs)", *operation, ok, result.c_str()), sender);
  });
}

std::uint64_t AgentEndpoint::track(const std::string& sender, const Peer& who, std::function<std::uint64_t(Done)> start) {
  auto slot = std::make_shared<std::uint64_t>(0);
  const std::uint64_t operation = start([this, sender, slot](bool ok, std::string result) {
    complete(sender, slot, ok, std::move(result));
  });
  *slot = operation;
  operations_[operation] = {sender, who.client};
  return operation;
}

void AgentEndpoint::ownerGone(const std::string& name) {
  peers_.erase(name);
  if (!broker_) return;
  if (confirmer_ == name) { confirmer_.clear(); broker_->setConfirmerPresent(false); }
  if (manager_ == name) manager_.clear();
  std::vector<std::pair<std::uint64_t, OperationOwner>> mine;
  for (const auto& [operation, owner] : operations_) if (owner.sender == name) mine.emplace_back(operation, owner);
  for (const auto& [operation, owner] : mine) broker_->cancel(owner.client, operation);
}

void AgentEndpoint::tick() { if (broker_) broker_->expire(); }

void AgentEndpoint::method(const std::string& sender, const std::string& name, GVariant* parameters,
                           GDBusMethodInvocation* invocation) {
  auto reply = [&](GVariant* value) { g_dbus_method_invocation_return_value(invocation, value); };
  try {
    if (!broker_) throw Failure("The agent surface is unavailable: " + unavailable_);
    const Json args = fromVariant(parameters);
    const Peer& who = peer(sender);

    if (name == "Hello") {
      peers_.at(sender).label = args.at(0).get<std::string>().substr(0, 64);
      reply(g_variant_new("(@a{sv})", toVariant({{"client", who.client}, {"tiers", tiersJson(grants_->tiersFor(who.client))},
                                                    {"confirmer", !confirmer_.empty()}})));
    } else if (name == "GetSnapshot") {
      reply(g_variant_new("(@a{sv})", toVariant({{"available", true}, {"client", who.client},
        {"tiers", tiersJson(grants_->tiersFor(who.client))}, {"confirmer", !confirmer_.empty()}})));
    } else if (name == "ListTools") {
      Json tools = Json::array();
      for (const auto& command : broker_->policy().commands()) {
        const Tier tier = broker_->policy().tierFor(command);
        if (grants_->allows(who.client, tier)) tools.push_back({{"name", command}, {"tier", std::string(tierName(tier))}});
      }
      reply(g_variant_new("(@av)", toVariant(tools)));
    } else if (name == "RequestAccess") {
      const auto tiers = tiersFrom(args.at(0), true);
      const auto label = peers_.at(sender).label.empty() ? who.exe : peers_.at(sender).label;
      reply(g_variant_new("(t)", track(sender, who, [&](Done done) {
        return broker_->requestAccess(who.client, label, tiers, std::move(done));
      })));
    } else if (name == "Action") {
      const auto tool = args.at(0).get<std::string>();
      const Json& options = args.at(1);
      Request request{who.client, tool, stringsFrom(options, "argv"), stringsFrom(options, "literal"),
                      options.contains("reason") && options.at("reason").is_string() ? options.at("reason").get<std::string>().substr(0, 200) : ""};
      reply(g_variant_new("(t)", track(sender, who, [&](Done done) { return broker_->submit(request, std::move(done)); })));
    } else if (name == "Cancel") {
      const auto operation = args.at(0).get<std::uint64_t>();
      const auto found = operations_.find(operation);
      if (found == operations_.end() || found->second.sender != sender || !broker_->cancel(who.client, operation))
        throw Invalid("There is no cancellable operation with that id");
      reply(nullptr);
    } else if (name == "RegisterConfirmer") {
      // Only the registered shell may confirm: confirmation is a human decision shown in shell UI.
      if (desktopOwner_().empty() || desktopOwner_() != sender) throw Invalid("Only the registered shell may confirm agent actions");
      if (!confirmer_.empty() && confirmer_ != sender) throw Failure("A confirmer is already registered");
      confirmer_ = sender;
      broker_->setConfirmerPresent(true);
      reply(nullptr);
    } else if (name == "Confirm") {
      if (confirmer_.empty() || confirmer_ != sender) throw Invalid("Only the registered confirmer may answer confirmations");
      std::optional<std::set<Tier>> narrowed;
      if (args.at(2).is_object() && args.at(2).contains("tiers")) narrowed = tiersFrom(args.at(2).at("tiers"), true);
      if (!broker_->confirm(args.at(0).get<std::uint64_t>(), args.at(1).get<bool>(), narrowed))
        throw Invalid("There is no pending confirmation with that id");
      reply(nullptr);
    } else if (name == "RegisterManager") {
      if (std::filesystem::path(who.exe).filename() != "helix-settings") throw Invalid("Only Helix Settings may manage agent access");
      if (!manager_.empty() && manager_ != sender) throw Failure("A manager is already registered");
      manager_ = sender;
      reply(nullptr);
    } else if (name == "GetGrants" || name == "SetGrant" || name == "RevokeClient" || name == "GetAudit") {
      if (manager_.empty() || manager_ != sender) throw Invalid("Only the registered manager may do that");
      if (name == "GetGrants") {
        Json out = Json::array();
        for (const auto& client : grants_->clients()) out.push_back({{"client", client}, {"tiers", tiersJson(grants_->tiersFor(client))}});
        reply(g_variant_new("(@av)", toVariant(out)));
      } else if (name == "GetAudit") {
        const auto since = args.at(0).get<std::uint64_t>();
        const auto limit = std::min<std::uint64_t>(args.at(1).get<std::uint64_t>(), 500);
        std::vector<Json> records;
        std::ifstream in(auditFile_);
        for (std::string line; std::getline(in, line);) {
          try {
            auto record = Json::parse(line);
            std::tm utc{};
            if (strptime(record.at("time").get<std::string>().c_str(), "%Y-%m-%dT%H:%M:%SZ", &utc) &&
                static_cast<std::uint64_t>(timegm(&utc)) >= since) records.push_back(std::move(record));
          } catch (const std::exception&) {}  // skip a damaged line rather than hide the rest
        }
        Json out = Json::array();
        for (std::size_t i = records.size() > limit ? records.size() - limit : 0; i < records.size(); ++i) out.push_back(records[i]);
        reply(g_variant_new("(@av)", toVariant(out)));
      } else {
        const auto client = args.at(0).get<std::string>();
        if (name == "SetGrant") grants_->set(client, tiersFrom(args.at(1)));
        else grants_->revoke(client);
        if (auto saved = grants_->save(); !saved) {
          (void)grants_->load();  // drop the unsaved change
          throw Failure(saved.error());
        }
        emit("Changed", nullptr, {});
        reply(nullptr);
      }
    } else {
      throw Invalid("Unknown agent method");
    }
  } catch (const Invalid& error) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.InvalidArgument", error.what());
  } catch (const std::invalid_argument& error) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.InvalidArgument", error.what());
  } catch (const std::exception& error) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Desktop.Error.Failed", error.what());
  }
}

}  // namespace helix::agent
