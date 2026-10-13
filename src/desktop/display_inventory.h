#pragma once

#include "desktop_api/protocol.h"
#include "system/display_config.h"

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>

namespace helix {
  // Read-only inventory. Display mutations remain owned by the existing shell backend.
  class DisplayInventory : public std::enable_shared_from_this<DisplayInventory> {
    GSubprocess* process = nullptr;
    GCancellable* cancellation = nullptr;
    guint deadline = 0;
    bool permitted = false;
    std::chrono::steady_clock::time_point next{};
    Json state = {{"available", false}, {"outputs", Json::array()}, {"capabilities", Json::array()}};
    void publish(Json value) {
      if (state == value)
        return;
      state = std::move(value);
      if (changed)
        changed();
    }

  public:
    std::function<void()> changed;
    ~DisplayInventory() {
      if (deadline)
        g_source_remove(deadline);
      if (process) {
        g_subprocess_force_exit(process);
        g_object_unref(process);
      }
      if (cancellation) {
        g_cancellable_cancel(cancellation);
        g_object_unref(cancellation);
      }
    }
    void shutdown() {
      changed = {};
      permitted = false;
      if (process) {
        g_cancellable_cancel(cancellation);
        g_subprocess_force_exit(process);
      }
    }
    const Json& snapshot() const { return state; }
    void refresh(bool desktop) {
      permitted = desktop && g_getenv("NIRI_SOCKET") && *g_getenv("NIRI_SOCKET");
      if (!permitted) {
        if (process) {
          g_cancellable_cancel(cancellation);
          g_subprocess_force_exit(process);
        }
        publish(
            {{"available", false},
             {"outputs", Json::array()},
             {"capabilities", Json::array()},
             {"reason", "Display inventory requires an active Helix desktop and its compositor connection."}}
        );
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (process || now < next)
        return;
      next = now + std::chrono::seconds(3);
      GError* spawnError = nullptr;
      process = g_subprocess_new(
          GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &spawnError, "niri",
          "msg", "--json", "outputs", nullptr
      );
      if (!process) {
        if (spawnError)
          g_error_free(spawnError);
        publish(
            {{"available", false},
             {"outputs", Json::array()},
             {"capabilities", Json::array()},
             {"reason", "The display backend is unavailable."}}
        );
        return;
      }
      cancellation = g_cancellable_new();
      auto* requestOwner = new std::shared_ptr<DisplayInventory>(shared_from_this());
      deadline = g_timeout_add_seconds(
          2,
          +[](gpointer data) -> gboolean {
            auto& self = *static_cast<DisplayInventory*>(data);
            self.deadline = 0;
            g_cancellable_cancel(self.cancellation);
            g_subprocess_force_exit(self.process);
            return G_SOURCE_REMOVE;
          },
          this
      );
      g_subprocess_communicate_utf8_async(
          process, nullptr, cancellation,
          +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::shared_ptr<DisplayInventory>> owner(
                static_cast<std::shared_ptr<DisplayInventory>*>(data)
            );
            auto& self = **owner;
            gchar* output = nullptr;
            GError* error = nullptr;
            const bool ok =
                g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, nullptr, &error);
            Json value = {
                {"available", false},
                {"outputs", Json::array()},
                {"capabilities", Json::array()},
                {"reason", "The display backend is unavailable. Settings will retry automatically."}
            };
            if (ok && g_subprocess_get_successful(G_SUBPROCESS(source)))
              try {
                const auto raw = Json::parse(output ? output : "");
                if (!raw.is_object())
                  throw std::invalid_argument("Invalid output inventory");
                for (const auto& item : raw)
                  if (!item.is_object() || !item.contains("name") || !item.at("name").is_string())
                    throw std::invalid_argument("Invalid output inventory");
                Json outputs = Json::array();
                // niri CLI returns the map directly; the shared parser expects the IPC envelope.
                for (const auto& item : display_config::parseOutputsReply(Json{{"Outputs", raw}}))
                  outputs.push_back(
                      {{"name", item.name},
                       {"label", display_config::displayLabel(item)},
                       {"enabled", item.enabled},
                       {"width", item.current.width},
                       {"height", item.current.height},
                       {"refresh-millihertz", item.current.refreshMilliHz},
                       {"scale", item.scale},
                       {"x", item.x},
                       {"y", item.y},
                       {"logical-width", item.logicalWidth},
                       {"logical-height", item.logicalHeight},
                       {"transform", display_config::jsonName(item.transform)},
                       {"vrr-supported", item.vrrSupported},
                       {"vrr-enabled", item.vrrEnabled}}
                  );
                value = {{"available", true}, {"outputs", outputs}, {"capabilities", Json::array({"inventory"})}};
              } catch (...) {
              }
            if (error)
              g_error_free(error);
            g_free(output);
            if (self.deadline) {
              g_source_remove(self.deadline);
              self.deadline = 0;
            }
            g_clear_object(&self.process);
            g_clear_object(&self.cancellation);
            if (self.permitted)
              self.publish(std::move(value));
          },
          requestOwner
      );
    }
  };
} // namespace helix
