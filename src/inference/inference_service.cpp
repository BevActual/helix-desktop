#include "inference/inference_service.h"

#include "desktop_api/peer_identity.h"
#include "desktop_api/protocol.h"
#include "inference/protocol.h"

#include <algorithm>
#include <set>

namespace helix::inference {

namespace {

using Json = helix::Json;

struct Invalid : std::invalid_argument { using std::invalid_argument::invalid_argument; };

struct Task { std::function<void()> run; };

Json outcomeJson(const Outcome& outcome) {
  return {{"ok", outcome.ok}, {"text", outcome.text}, {"error", outcome.error}, {"cancelled", outcome.cancelled},
          {"truncated", outcome.truncated}, {"prompt_tokens", outcome.promptTokens}, {"completion_tokens", outcome.completionTokens}};
}

ChatRequest parseChat(const Json& args, const std::string& client) {
  static const std::set<std::string> known{"messages", "model", "schema", "max_tokens", "temperature", "priority"};
  if (!args.is_object()) throw Invalid("arguments must be a dictionary");
  for (const auto& [key, value] : args.items())
    if (!known.contains(key)) throw Invalid("unknown argument: " + key);

  ChatRequest request;
  request.client = client;
  if (!args.contains("messages")) throw Invalid("messages is required");
  request.messages = args.at("messages");  // the queue validates the shape
  if (args.contains("model")) {
    if (!args.at("model").is_string()) throw Invalid("model must be a string");
    request.model = args.at("model").get<std::string>();
  }
  if (args.contains("schema")) {
    if (!args.at("schema").is_string()) throw Invalid("schema must be a string of JSON");
    request.jsonSchema = args.at("schema").get<std::string>();
  }
  if (args.contains("max_tokens")) {
    if (!args.at("max_tokens").is_number_integer()) throw Invalid("max_tokens must be an integer");
    const auto value = args.at("max_tokens").get<std::int64_t>();
    request.maxTokens = value < 1 ? 1U : static_cast<std::uint32_t>(std::min<std::int64_t>(value, 1'000'000));
  }
  if (args.contains("temperature")) {
    if (!args.at("temperature").is_number()) throw Invalid("temperature must be a number");
    request.temperature = args.at("temperature").get<double>();
  }
  if (args.contains("priority")) {
    const auto priority = args.at("priority").is_string() ? args.at("priority").get<std::string>() : std::string();
    if (priority == "background") request.priority = Priority::Background;
    else if (priority != "interactive") throw Invalid("priority must be interactive or background");
  }
  return request;
}

}  // namespace

InferenceService::InferenceService(GDBusConnection* bus, Config config, std::shared_ptr<Backend> backend)
    : bus_(bus), config_(std::move(config)), backend_(std::move(backend)) {
  queue_ = std::make_unique<InferenceQueue>(
      backend_, config_.limits,
      [this](std::uint64_t operation, const std::string& text) {
        post([this, operation, text] {
          if (const auto owner = owners_.find(operation); owner != owners_.end())
            emit("Delta", g_variant_new("(ts)", operation, text.c_str()), owner->second.sender);
        });
      },
      [this](std::uint64_t operation, const Outcome& outcome) {
        post([this, operation, outcome] {
          const auto owner = owners_.find(operation);
          if (owner == owners_.end()) return;
          emit("Completed", g_variant_new("(tbs)", operation, outcome.ok, outcomeJson(outcome).dump().c_str()), owner->second.sender);
          owners_.erase(owner);
        });
      });
}

InferenceService::~InferenceService() { queue_.reset(); }

void InferenceService::post(std::function<void()> task) {
  g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
    std::unique_ptr<Task> job(static_cast<Task*>(data));
    job->run();
    return G_SOURCE_REMOVE;
  }, new Task{std::move(task)}, nullptr);
}

void InferenceService::emit(const char* signal, GVariant* value, const std::string& destination) {
  g_dbus_connection_emit_signal(bus_, destination.empty() ? nullptr : destination.c_str(), objectPath, interfaceName, signal, value, nullptr);
}

const std::string& InferenceService::clientOf(const std::string& sender) {
  if (const auto found = clients_.find(sender); found != clients_.end()) return found->second;
  // Process id only: this service runs sandboxed, in its own user namespace, where other processes' executables are unreadable.
  const auto identity = resolvePeer(bus_, sender, PeerDetail::ProcessOnly);
  if (!identity) {
    if (identity.error().wrongUser) throw Invalid(identity.error().message);
    throw std::runtime_error(identity.error().message);
  }
  return clients_.emplace(sender, identity->client).first->second;
}

void InferenceService::ownerGone(const std::string& name) {
  clients_.erase(name);
  std::vector<std::pair<std::uint64_t, std::string>> mine;
  for (const auto& [operation, owner] : owners_) if (owner.sender == name) mine.emplace_back(operation, owner.client);
  for (const auto& [operation, client] : mine) queue_->cancel(client, operation);
}

void InferenceService::tick() {
  if (config_.idleUnload.count() > 0 && queue_->unloadIfIdle(config_.idleUnload)) emit("Changed", nullptr, {});
}

void InferenceService::method(const std::string& sender, const std::string& name, GVariant* parameters,
                              GDBusMethodInvocation* invocation) {
  try {
    const Json args = fromVariant(parameters);
    const std::string& client = clientOf(sender);

    if (name == "GetSnapshot") {
      Json snapshot = queue_->snapshot();
      snapshot["model_dir"] = config_.modelDir;
      snapshot["idle_unload_seconds"] = config_.idleUnload.count();
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant(snapshot)));
    } else if (name == "ListModels") {
      Json models = Json::array();
      for (const auto& model : backend_->models()) models.push_back({{"id", model.id}, {"bytes", model.bytes}, {"loaded", model.loaded}});
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(@av)", toVariant(models)));
    } else if (name == "Action") {
      const auto action = args.at(0).get<std::string>();
      if (action != "chat") throw Invalid("unknown action: " + action);
      auto submitted = queue_->submit(parseChat(args.at(1), client));
      if (!submitted) throw Invalid(submitted.error());
      owners_[*submitted] = {sender, client};  // callbacks reach the main loop only after this handler returns
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", *submitted));
    } else if (name == "Cancel") {
      const auto operation = args.at(0).get<std::uint64_t>();
      const auto owner = owners_.find(operation);
      if (owner == owners_.end() || owner->second.sender != sender || !queue_->cancel(client, operation))
        throw Invalid("There is no cancellable operation with that id");
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
      throw Invalid("Unknown method");
    }
  } catch (const std::invalid_argument& error) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Inference.Error.InvalidArgument", error.what());
  } catch (const std::exception& error) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.helix.Inference.Error.Failed", error.what());
  }
}

}  // namespace helix::inference
