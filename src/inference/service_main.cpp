#include "helix_inference_introspection.h"
#include "inference/fake_backend.h"
#include "inference/inference_service.h"
#include "inference/protocol.h"
#include "inference/transcriber.h"

#ifdef HELIX_HAVE_LLAMA
#include "inference/llama_backend.h"
#endif

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <glib-unix.h>
#include <iostream>

namespace {

  namespace fs = std::filesystem;
  using namespace helix::inference;

  std::string env(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
  }

  fs::path defaultModelDir() {
    if (const auto data = env("XDG_DATA_HOME"); !data.empty())
      return fs::path(data) / "helix" / "models";
    if (const auto home = env("HOME"); !home.empty())
      return fs::path(home) / ".local/share/helix/models";
    return "/nonexistent";
  }

  struct App {
    GMainLoop* loop = nullptr;
    GDBusNodeInfo* info = nullptr;
    std::unique_ptr<InferenceService> service;
    std::shared_ptr<Backend> backend;
    InferenceService::Config config;
    std::shared_ptr<Transcriber> transcriber;
    std::shared_ptr<ImageGenerator> imageGenerator;
    guint ownerWatch = 0, tick = 0;
    std::vector<guint> registrations;
  };

} // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN); // an engine worker may fail before reading its input
  App app;
  app.config.modelDir = env("HELIX_MODEL_DIR", defaultModelDir().string());
  app.config.idleUnload = std::chrono::seconds(std::atoi(env("HELIX_INFERENCE_IDLE_SECONDS", "300").c_str()));

  const std::string kind = env("HELIX_INFERENCE_BACKEND", "llama");
  if (kind == "fake") { // scripted replies, for tests of the D-Bus layer
    auto fake = std::make_shared<FakeBackend>();
    fake->reply = env("HELIX_FAKE_REPLY", "hello from the fake backend");
    fake->perChunk = std::chrono::milliseconds(std::atoi(env("HELIX_FAKE_DELAY_MS", "0").c_str()));
    app.backend = fake;
  } else {
#ifdef HELIX_HAVE_LLAMA
    app.backend = std::make_shared<LlamaBackend>(LlamaBackend::Config{.modelDir = app.config.modelDir});
#else
    std::cerr << "helix-inference: built without an engine; set HELIX_INFERENCE_BACKEND=fake to test\n";
    return 1;
#endif
  }

  // Speech to text: the worker program sits beside this one (or where HELIX_TRANSCRIBE_BIN says); models are in
  // <models>/speech.
  if (kind == "fake"
      && env("HELIX_TRANSCRIBER") != "process") { // the scripted one, unless a test wants the real worker
    app.transcriber = std::make_shared<FakeTranscriber>();
  } else {
    std::error_code ec;
    fs::path worker = env("HELIX_TRANSCRIBE_BIN");
    if (worker.empty())
      worker = fs::read_symlink("/proc/self/exe", ec).parent_path() / "helix-transcribe";
    app.transcriber = std::make_shared<ProcessTranscriber>(worker, fs::path(app.config.modelDir) / "speech");
  }

  if (kind == "fake" && env("HELIX_IMAGE_GENERATOR") != "process")
    app.imageGenerator = std::make_shared<FakeImageGenerator>();
  else {
    std::error_code ec;
    fs::path worker = env("HELIX_IMAGE_BIN");
    if (worker.empty())
      worker = fs::read_symlink("/proc/self/exe", ec).parent_path() / "helix-image";
    app.imageGenerator = std::make_shared<ProcessImageGenerator>(worker, fs::path(app.config.modelDir) / "image");
  }
  app.loop = g_main_loop_new(nullptr, false);
  GError* error = nullptr;
  app.info = g_dbus_node_info_new_for_xml(helix::protocols::inferenceIntrospectionXml, &error);
  if (!app.info) {
    std::cerr << "helix-inference: bad interface: " << (error ? error->message : "?") << "\n";
    return 1;
  }

  static const GDBusInterfaceVTable vtable = {
      [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* name, GVariant* parameters,
         GDBusMethodInvocation* invocation,
         gpointer data) { static_cast<App*>(data)->service->method(sender, name, parameters, invocation); },
      nullptr,
      nullptr,
      {nullptr}
  };

  const guint owned = g_bus_own_name(
      G_BUS_TYPE_SESSION, busName, G_BUS_NAME_OWNER_FLAGS_NONE,
      [](GDBusConnection* bus, const gchar*, gpointer data) {
        auto& self = *static_cast<App*>(data);
        GError* failure = nullptr;
        self.service = std::make_unique<InferenceService>(bus, self.config, self.backend);
        self.service->setTranscriber(self.transcriber);
        self.service->setImageGenerator(self.imageGenerator);
        if (!g_dbus_connection_register_object(
                bus, objectPath, self.info->interfaces[0], &vtable, &self, nullptr, &failure
            )) {
          std::cerr << "helix-inference: could not register: " << (failure ? failure->message : "?") << "\n";
          g_main_loop_quit(self.loop);
          return;
        }
        self.ownerWatch = g_dbus_connection_signal_subscribe(
            bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* value,
               gpointer user) {
              const gchar *name, *oldOwner, *newOwner;
              g_variant_get(value, "(&s&s&s)", &name, &oldOwner, &newOwner);
              if (!*newOwner)
                static_cast<App*>(user)->service->ownerGone(name);
            },
            &self, nullptr
        );
        self.tick = g_timeout_add_seconds(
            15,
            [](gpointer user) -> gboolean {
              static_cast<App*>(user)->service->tick();
              return G_SOURCE_CONTINUE;
            },
            &self
        );
      },
      nullptr,
      [](GDBusConnection*, const gchar*, gpointer data) {
        g_main_loop_quit(static_cast<App*>(data)->loop);
      }, // lost the name
      &app, nullptr
  );

  const auto stop = [](gpointer data) -> gboolean {
    g_main_loop_quit(static_cast<App*>(data)->loop);
    return G_SOURCE_CONTINUE;
  };
  const guint term = g_unix_signal_add(SIGTERM, stop, &app);
  const guint interrupt = g_unix_signal_add(SIGINT, stop, &app);
  g_main_loop_run(app.loop);
  g_source_remove(term);
  g_source_remove(interrupt);
  app.service.reset(); // stops the worker before the bus goes away
  g_bus_unown_name(owned);
  return 0;
}
