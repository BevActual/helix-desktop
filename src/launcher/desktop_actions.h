#pragma once

#include <string>
#include <string_view>
#include <vector>

class Bar;
class ConfigService;
class CompositorPlatform;
class NotificationManager;
class PanelManager;
struct wl_output;

// Semantic desktop verbs shared by the launcher, Caps Lock quick insert, and
// `noctalia msg app-launch`. Humans and agents run the same ids.
namespace desktop_actions {

  struct Services {
    PanelManager* panels = nullptr;
    NotificationManager* notifications = nullptr;
    CompositorPlatform* compositor = nullptr;
    Bar* bar = nullptr;
    ConfigService* config = nullptr;
  };

  struct Action {
    std::string id;
    std::string title;
    std::string subtitle;
    std::string glyph;
  };

  struct Request {
    std::string id;
    wl_output* output = nullptr;
    std::string sourceBarName;
  };

  [[nodiscard]] std::string focusedWindowLabel(const CompositorPlatform* compositor);
  [[nodiscard]] std::string activeWorkspaceLabel(const CompositorPlatform* compositor);

  // Closes a compositor window. An empty id closes the focused window, or the
  // last real window when a shell panel is holding the keyboard.
  [[nodiscard]] bool closeWindow(CompositorPlatform* compositor, std::string_view windowId, std::string& error);

  [[nodiscard]] std::vector<Action> query(std::string_view text, const Services& services);

  // Runs on the next main-loop turn so closing the launcher or quick insert
  // cannot undo a panel open.
  void schedule(const Services& services, Request request);

  // Launches an installed application by desktop id or name. On success returns
  // the display name. On failure returns empty and sets `error`.
  [[nodiscard]] std::string launchQuery(
      CompositorPlatform* compositor, ConfigService* config, std::string_view query, std::string& error
  );

} // namespace desktop_actions
