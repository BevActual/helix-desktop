#include "inference/inference_service.h"
#include "inference/transcriber.h"

#include <algorithm>
#include <cstring>

#include "desktop_api/peer_identity.h"
#include "desktop_api/protocol.h"
#include "inference/image_input.h"
#include "inference/protocol.h"
#include "inference/routing.h"

#include <algorithm>
#include <set>

namespace helix::inference {

namespace {

using Json = helix::Json;

struct Invalid : std::invalid_argument { using std::invalid_argument::invalid_argument; };

struct Task { std::function<void()> run; };

Json outcomeJson(const Outcome& outcome) {
  Json result = {{"ok", outcome.ok}, {"text", outcome.text}, {"error", outcome.error}, {"cancelled", outcome.cancelled},
          {"truncated", outcome.truncated}, {"prompt_tokens", outcome.promptTokens},
          {"cached_prompt_tokens", outcome.cachedPromptTokens}, {"completion_tokens", outcome.completionTokens},
          {"language", outcome.language}};
  if (!outcome.imagePng.empty()) result["image"] = {{"png", outcome.imagePng}, {"model", outcome.imageModel}, {"prompt", outcome.imagePrompt}, {"width", outcome.imageWidth}, {"height", outcome.imageHeight}, {"seed", outcome.imageSeed}};
  return result;
}

ChatRequest parseChat(const Json& args, const std::string& client, const RoutingTable& routing,
                      std::vector<ImageInput> images = {}) {
  static const std::set<std::string> known{"messages", "model", "schema", "max_tokens", "temperature", "priority",
                                           "task"};
  if (!args.is_object()) throw Invalid("arguments must be a dictionary");
  for (const auto& [key, value] : args.items())
    if (!known.contains(key)) throw Invalid("unknown argument: " + key);

  ChatRequest request;
  request.client = client;
  if (!args.contains("messages")) throw Invalid("messages is required");
  request.messages = args.at("messages");  // the queue validates the shape
  request.images = std::move(images);
  // Every image part must name a picture that was sent, and every picture must be used.
  {
    std::set<std::size_t> referenced;
    for (const auto& message : request.messages) {
      if (!message.is_object() || !message.contains("content") || !message.at("content").is_array()) continue;
      for (const auto& part : message.at("content")) {
        if (!part.is_object()) throw Invalid("message parts must be objects");
        const auto type = part.value("type", std::string());
        if (type == "text") continue;
        if (type != "image") throw Invalid("unknown message part: " + type);
        if (!part.contains("index") || !part.at("index").is_number_unsigned()) throw Invalid("an image part needs an index");
        const auto index = part.at("index").get<std::size_t>();
        if (index >= request.images.size()) throw Invalid("an image part refers to a picture that was not sent");
        referenced.insert(index);
      }
    }
    if (referenced.size() != request.images.size()) throw Invalid("every picture sent must be used in a message");
  }
  std::string task = "chat";
  if (args.contains("task")) {
    if (!args.at("task").is_string()) throw Invalid("task must be a string");
    task = args.at("task").get<std::string>();
    static const std::set<std::string> tasks{"act", "chat", "summarize", "vision", "code"};
    if (!tasks.contains(task)) throw Invalid("task must be act, chat, summarize, vision or code");
  }
  if (args.contains("model")) {
    if (!args.at("model").is_string()) throw Invalid("model must be a string");
    request.model = args.at("model").get<std::string>();
  }
  // Local model slots: when the client omits model, use the profile from inference-routing.toml.
  if (request.model.empty()) {
    if (!routing.isLocal(task))
      throw Invalid(
          "task \"" + task +
          "\" is routed to a non-local backend; the sandboxed engine only runs local GGUF "
          "(use Mobius for LAN/cloud talk)");
    request.model = routing.route(task).model;
  }
  if (args.contains("schema")) {
    if (!args.at("schema").is_string()) throw Invalid("schema must be a string of JSON");
    // Only the on-device Act slot may emit tool JSON (grammar). Refuse schemas on talk tasks.
    if (task != "act") throw Invalid("schema is only allowed for task \"act\"");
    if (!routing.isLocal(task)) throw Invalid("schema requires a local act backend");
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
  queue_->onLoading([this](std::uint64_t operation, const std::string& model, double fraction) {
    post([this, operation, model, fraction] {
      if (const auto owner = owners_.find(operation); owner != owners_.end())
        emit("Loading", g_variant_new("(tsd)", operation, model.c_str(), fraction), owner->second.sender);
    });
  });
  queue_->onProgress([this](std::uint64_t operation, double fraction, const std::string& stage) {
    post([this, operation, fraction, stage] {
      if (const auto owner = owners_.find(operation); owner != owners_.end())
        emit("Progress", g_variant_new("(tds)", operation, fraction, stage.c_str()), owner->second.sender);
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
    // Pictures (aay, "images") and a recording (ay, "audio") travel as byte arrays and are taken out before the rest becomes
    // JSON: millions of numbers in a JSON array would cost more than the data itself.
    std::vector<std::vector<std::uint8_t>> pictures;
    std::vector<std::uint8_t> recording;
    bool hasRecording = false;
    GVariant* converted = parameters;
    GVariant* stripped = nullptr;
    if (name == "Action" && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv})"))) {
      GVariant* dictionary = g_variant_get_child_value(parameters, 1);
      struct Drop { GVariant* v; ~Drop() { g_variant_unref(v); } } dropDictionary{dictionary};
      const auto unwrap = [](GVariant* value) {  // a{sv} holds its value in a variant; take it out
        return g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT) ? g_variant_get_variant(value) : g_variant_ref(value);
      };
      GVariant* list = g_variant_lookup_value(dictionary, "images", nullptr);
      GVariant* audio = g_variant_lookup_value(dictionary, "audio", nullptr);
      struct Drop2 { GVariant* v; ~Drop2() { if (v) g_variant_unref(v); } } dropList{list}, dropAudio{audio};
      if (list) {
        GVariant* inner = unwrap(list);
        struct Drop3 { GVariant* v; ~Drop3() { g_variant_unref(v); } } dropInner{inner};
        if (!g_variant_is_of_type(inner, G_VARIANT_TYPE("aay"))) throw Invalid("images must be a list of byte arrays");
        const gsize count = g_variant_n_children(inner);
        std::size_t total = 0;
        if (count > kMaxImages) throw Invalid("too many pictures");
        for (gsize i = 0; i < count; ++i) {
          GVariant* bytes = g_variant_get_child_value(inner, i);
          struct Drop4 { GVariant* v; ~Drop4() { g_variant_unref(v); } } dropBytes{bytes};
          gsize length = 0;
          const auto* data = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(bytes, &length, 1));
          total += length;
          if (length > kMaxImageBytes || total > kMaxImageBytesTotal) throw Invalid("the pictures are too large");
          pictures.emplace_back(data, data + length);
        }
      }
      if (audio) {
        GVariant* inner = unwrap(audio);
        struct Drop3 { GVariant* v; ~Drop3() { g_variant_unref(v); } } dropInner{inner};
        if (!g_variant_is_of_type(inner, G_VARIANT_TYPE("ay"))) throw Invalid("audio must be a byte array");
        gsize length = 0;
        const auto* data = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(inner, &length, 1));
        if (length > kMaxAudioBytes) throw Invalid("the recording is too long");
        recording.assign(data, data + length);
        hasRecording = true;
      }
      if (list || audio) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        GVariantIter iter;
        g_variant_iter_init(&iter, dictionary);
        const char* key;
        GVariant* value;
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
          const std::string k = key;
          if (k != "images" && k != "audio") g_variant_builder_add(&builder, "{sv}", key, value);
          g_variant_unref(value);
        }
        GVariant* action = g_variant_get_child_value(parameters, 0);
        stripped = g_variant_ref_sink(g_variant_new("(@s@a{sv})", action, g_variant_builder_end(&builder)));
        converted = stripped;
      }
    }
    struct Unref { GVariant* v; ~Unref() { if (v) g_variant_unref(v); } } cleanup{stripped};

    const Json args = fromVariant(converted);
    const std::string& client = clientOf(sender);

    if (name == "GetSnapshot") {
      Json snapshot = queue_->snapshot();
      snapshot["model_dir"] = config_.modelDir;
      snapshot["idle_unload_seconds"] = config_.idleUnload.count();
      const RoutingTable routing = loadRouting();
      Json backends = Json::array();
      for (const auto& b : routing.backends)
        backends.push_back(
            {{"id", b.id}, {"kind", b.kind}, {"base_url", b.baseUrl}, {"label", b.label}});
      snapshot["backends"] = backends;
      Json profiles = Json::object();
      for (const auto& [profile, route] : routing.profiles)
        profiles[profile] = {{"backend", route.backend}, {"model", route.model}};
      snapshot["profiles"] = profiles;
      snapshot["allow_remote_on_battery_saver"] = routing.allowRemoteOnBatterySaver;
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", toVariant(snapshot)));
    } else if (name == "ListModels") {
      Json models = Json::array();
      for (const auto& model : backend_->models())
        models.push_back({{"id", model.id}, {"bytes", model.bytes}, {"loaded", model.loaded}, {"vision", model.vision}});
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(@av)", toVariant(models)));
    } else if (name == "Action") {
      const auto action = args.at(0).get<std::string>();
      if (action == "image") {
        if (hasRecording || !pictures.empty()) throw Invalid("image generation currently accepts a text prompt only");
        const Json& options = args.at(1);
        for (const auto& [key, unused] : options.items()) {
          (void)unused;
          if (key != "prompt" && key != "model" && key != "width" && key != "height" && key != "steps" && key != "seed") throw Invalid("unknown image argument: " + key);
        }
        if (!options.contains("prompt") || !options.at("prompt").is_string()) throw Invalid("an image prompt is required");
        ChatRequest request;
        request.client = client; request.generateImage = true; request.imagePrompt = options.at("prompt").get<std::string>();
        if (options.contains("model")) {
          if (!options.at("model").is_string()) throw Invalid("image model must be a file name");
          request.model = options.at("model").get<std::string>();
        }
        const auto number = [&](const char* key, std::uint32_t fallback, std::uint32_t max) {
          if (!options.contains(key)) return fallback;
          const auto& value = options.at(key);
          if (!value.is_number_integer() || value < 0 || value > max) throw Invalid(std::string(key) + " must be a bounded non-negative integer");
          return value.get<std::uint32_t>();
        };
        request.imageWidth = number("width", 512, 768); request.imageHeight = number("height", 512, 768);
        request.imageSteps = number("steps", 4, 8); request.imageSeed = number("seed", 0, 2147483647);
        auto submitted = queue_->submit(std::move(request));
        if (!submitted) throw Invalid(submitted.error());
        owners_[*submitted] = {sender, client};
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", *submitted));
        return;
      }
      if (action == "transcribe") {
        if (!hasRecording) throw Invalid("audio is required");
        if (recording.size() % 2 != 0 || recording.empty()) throw Invalid("audio must be 16-bit samples");
        const Json& options = args.at(1);
        for (const auto& [key, unused] : options.items()) {
          (void)unused;
          if (key != "language" && key != "priority") throw Invalid("unknown argument: " + key);
        }
        ChatRequest request;
        request.client = client;
        request.transcribe = true;
        request.audio.resize(recording.size() / 2);
        std::memcpy(request.audio.data(), recording.data(), recording.size());
        if (options.contains("language")) {
          const auto& value = options.at("language");
          const bool plain = value.is_string() && (value == "auto" || (value.get<std::string>().size() >= 2 && value.get<std::string>().size() <= 3 &&
              std::ranges::all_of(value.get<std::string>(), [](char c) { return c >= 'a' && c <= 'z'; })));
          if (!plain) throw Invalid("language must be auto or a language code like en");
          request.language = value.get<std::string>();
        }
        auto submitted = queue_->submit(std::move(request));
        if (!submitted) throw Invalid(submitted.error());
        owners_[*submitted] = {sender, client};
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", *submitted));
        return;
      }
      if (action != "chat") throw Invalid("unknown action: " + action);
      if (hasRecording) throw Invalid("unknown argument: audio");
      const RoutingTable routing = loadRouting();
      std::vector<ImageInput> decoded;
      for (const auto& bytes : pictures) {
        auto image = decodeImageInput(bytes.data(), bytes.size());
        if (!image) throw Invalid(image.error());
        decoded.push_back(std::move(*image));
      }
      auto submitted = queue_->submit(parseChat(args.at(1), client, routing, std::move(decoded)));
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
