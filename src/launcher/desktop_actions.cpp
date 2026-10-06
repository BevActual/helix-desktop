#include "launcher/desktop_actions.h"

#include "compositors/compositor_platform.h"
#include "compositors/workspace_backend.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "shell/bar/bar.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/wayland_toplevels.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>

namespace desktop_actions {
  namespace {

    constexpr Logger kLog("desktop-actions");

    struct Spec {
      std::string_view id;
      std::string_view titleKey;
      std::string_view subtitleKey;
      std::string_view glyph;
    };

    // Stable order for an empty query. Close-focused is inserted in front when
    // a window is focused, so the list leads with the thing on screen.
    constexpr Spec kSpecs[] = {
        {"notifications", "launcher.providers.actions.notifications", "launcher.providers.actions.notifications-sub",
         "bell"},
        {"control-center", "launcher.providers.actions.control-center", "launcher.providers.actions.control-center-sub",
         "adjustments"},
        {"settings", "launcher.providers.actions.settings", "launcher.providers.actions.settings-sub", "settings"},
        {"software", "launcher.providers.actions.software", "launcher.providers.actions.software-sub", "package"},
        {"monitor", "launcher.providers.actions.monitor", "launcher.providers.actions.monitor-sub", "activity"},
        {"dnd", "launcher.providers.actions.dnd", "launcher.providers.actions.dnd-sub", "bell-off"},
        {"workspace-next", "launcher.providers.actions.workspace-next", "launcher.providers.actions.workspace-next-sub",
         "arrow-right"},
        {"workspace-prev", "launcher.providers.actions.workspace-prev", "launcher.providers.actions.workspace-prev-sub",
         "arrow-left"},
        {"bar-toggle", "launcher.providers.actions.bar-toggle", "launcher.providers.actions.bar-toggle-sub",
         "layout-navbar"},
    };

    [[nodiscard]] std::optional<DesktopEntry> entryForId(std::string_view id) {
      for (const auto& entry : desktopEntries()) {
        if (entry.id == id && !entry.hidden && !entry.noDisplay && !entry.exec.empty()) {
          return entry;
        }
      }
      DesktopEntry synthetic;
      synthetic.id = std::string(id);
      if (id == "org.helix.Settings") {
        synthetic.name = "Helix Settings";
        synthetic.exec = "helix-settings";
      } else if (id == "org.helix.Software") {
        synthetic.name = "Helix Software";
        synthetic.exec = "helix-software";
      } else if (id == "org.helix.SystemMonitor") {
        synthetic.name = "Helix System Monitor";
        synthetic.exec = "helix-system-monitor";
      } else {
        return std::nullopt;
      }
      return synthetic;
    }

    [[nodiscard]] std::optional<DesktopEntry> resolveQuery(std::string_view query) {
      const std::string trimmed = StringUtils::trim(query);
      if (trimmed.empty()) {
        return std::nullopt;
      }
      const std::string lower = StringUtils::toLower(trimmed);
      const DesktopEntry* exact = nullptr;
      const DesktopEntry* best = nullptr;
      double bestScore = 0.0;
      for (const auto& entry : desktopEntries()) {
        if (entry.hidden || entry.noDisplay || entry.exec.empty()) {
          continue;
        }
        if (entry.idLower == lower || entry.nameLower == lower || entry.execLower == lower) {
          exact = &entry;
          break;
        }
        const double score = std::max(
            FuzzyMatch::score(lower, entry.nameLower),
            std::max(FuzzyMatch::score(lower, entry.idLower), FuzzyMatch::score(lower, entry.execLower))
        );
        if (FuzzyMatch::isMatch(score) && (best == nullptr || score > bestScore)) {
          best = &entry;
          bestScore = score;
        }
      }
      if (exact != nullptr) {
        return *exact;
      }
      if (best != nullptr) {
        return *best;
      }
      return entryForId(trimmed);
    }

    struct OpenWindow {
      std::string id;
      std::string title;
    };

    [[nodiscard]] std::optional<OpenWindow>
    openWindowFor(const CompositorPlatform* compositor, std::span<const std::string_view> ids) {
      if (compositor == nullptr) {
        return std::nullopt;
      }
      for (const auto& window : compositor->workspaceWindowAssignments()) {
        if (window.windowId.empty() || window.appId.empty()) {
          continue;
        }
        const std::string appId = StringUtils::toLower(window.appId);
        for (const std::string_view id : ids) {
          if (appId == id) {
            return OpenWindow{.id = window.windowId, .title = window.title.empty() ? window.appId : window.title};
          }
        }
      }
      return std::nullopt;
    }

    [[nodiscard]] std::span<const std::string_view> helixAppIds(std::string_view actionId) {
      static constexpr std::array<std::string_view, 2> kSettings{"org.helix.settings", "helix-settings"};
      static constexpr std::array<std::string_view, 2> kSoftware{"org.helix.software", "helix-software"};
      static constexpr std::array<std::string_view, 2> kMonitor{"org.helix.systemmonitor", "helix-system-monitor"};
      if (actionId == "settings") {
        return kSettings;
      }
      if (actionId == "software") {
        return kSoftware;
      }
      if (actionId == "monitor") {
        return kMonitor;
      }
      return {};
    }

    [[nodiscard]] std::string activeWorkspaceName(const CompositorPlatform* compositor) {
      if (compositor == nullptr) {
        return {};
      }
      const auto workspaces = compositor->workspaces(compositor->preferredInteractiveOutput());
      for (const auto& workspace : workspaces) {
        if (workspace.active) {
          return workspace.name.empty() ? workspace.id : workspace.name;
        }
      }
      return {};
    }

    [[nodiscard]] bool launchEntry(
        CompositorPlatform* compositor, ConfigService* config, const DesktopEntry& entry, std::string& error
    ) {
      desktop_entry_launch::LaunchOptions options;
      if (compositor != nullptr && compositor->hasXdgActivation()) {
        options.activationToken = compositor->requestActivationToken(nullptr);
      }
      if (config != nullptr) {
        options.runAsSystemdService = config->config().shell.launchAppsAsSystemdServices;
        options.customCommand = config->config().shell.launchAppsCustomCommand;
      }
      options.dbusActivatable = entry.dbusActivatable;
      options.dbusAppId = entry.id;
      if (!desktop_entry_launch::launchEntry(entry, options)) {
        error = "failed to launch " + (entry.name.empty() ? entry.id : entry.name);
        return false;
      }
      return true;
    }

    [[nodiscard]] bool stepWorkspace(CompositorPlatform& compositor, bool forward) {
      wl_output* output = compositor.preferredInteractiveOutput();
      const auto workspaces = compositor.workspaces(output);
      if (workspaces.empty()) {
        return false;
      }
      const auto active = std::ranges::find(workspaces, true, &Workspace::active);
      std::size_t target = 0;
      if (active == workspaces.end()) {
        target = forward ? 0 : workspaces.size() - 1;
      } else {
        const auto current = static_cast<std::size_t>(std::ranges::distance(workspaces.begin(), active));
        if (forward) {
          if (current + 1 >= workspaces.size()) {
            return true;
          }
          target = current + 1;
        } else {
          if (current == 0) {
            return true;
          }
          target = current - 1;
        }
      }
      compositor.activateWorkspace(output, workspaces[target]);
      return true;
    }

    [[nodiscard]] bool closeFocused(CompositorPlatform& compositor) {
      if (const auto active = compositor.activeToplevel(); active.has_value() && active->handle != nullptr) {
        ToplevelInfo info;
        info.title = active->title;
        info.appId = active->appId;
        info.identifier = active->identifier;
        info.handle = active->handle;
        info.exactIdentity = !active->identifier.empty();
        compositor.closeToplevelInfo(info);
        return true;
      }
      const auto recent = compositor.recentFocusedWindowId();
      if (!recent.has_value() || recent->empty()) {
        return false;
      }
      ToplevelInfo info;
      info.identifier = *recent;
      info.exactIdentity = true;
      compositor.closeToplevelInfo(info);
      return true;
    }

    [[nodiscard]] bool run(const Services& services, const Request& request, std::string& error) {
      const std::string_view id = request.id;
      if (id == "notifications" || id == "control-center") {
        if (services.panels == nullptr) {
          error = "panels are unavailable";
          return false;
        }
        const std::string_view context = id == "notifications" ? "notifications" : "home";
        services.panels->openPanel(
            "control-center",
            PanelOpenRequest{.output = request.output, .context = context, .sourceBarName = request.sourceBarName}
        );
        return true;
      }
      if (id == "settings" || id == "software" || id == "monitor") {
        if (const auto open = openWindowFor(services.compositor, helixAppIds(id)); open.has_value()) {
          if (services.compositor == nullptr) {
            error = "compositor is unavailable";
            return false;
          }
          services.compositor->focusCompositorWindow(open->id);
          return true;
        }
      }
      if (id == "settings" || id == "software") {
        if (services.panels == nullptr) {
          error = "panels are unavailable";
          return false;
        }
        services.panels->openSettings(id == "software" ? "software" : "");
        return true;
      }
      if (id == "monitor") {
        const auto entry = entryForId("org.helix.SystemMonitor");
        if (!entry.has_value()) {
          error = "System Monitor is not available";
          return false;
        }
        return launchEntry(services.compositor, services.config, *entry, error);
      }
      if (id == "dnd") {
        if (services.notifications == nullptr) {
          error = "notifications are unavailable";
          return false;
        }
        (void)services.notifications->toggleDoNotDisturb();
        return true;
      }
      if (id == "workspace-next" || id == "workspace-prev") {
        if (services.compositor == nullptr || !stepWorkspace(*services.compositor, id == "workspace-next")) {
          error = "no workspace to switch to";
          return false;
        }
        return true;
      }
      if (id == "bar-toggle") {
        if (services.bar == nullptr) {
          error = "bar is unavailable";
          return false;
        }
        const std::string result = services.bar->toggleVisibility();
        if (result.starts_with("error")) {
          error = StringUtils::trim(result);
          return false;
        }
        return true;
      }
      if (id == "close-focused") {
        return closeWindow(services.compositor, {}, error);
      }
      if (id.starts_with("app:")) {
        const auto entry = entryForId(id.substr(4));
        if (!entry.has_value()) {
          error = "application is not available";
          return false;
        }
        return launchEntry(services.compositor, services.config, *entry, error);
      }
      error = "unknown action";
      return false;
    }

    [[nodiscard]] Action makeAction(const Spec& spec, const Services& services) {
      Action action;
      action.id = std::string(spec.id);
      action.title = i18n::tr(spec.titleKey);
      action.glyph = std::string(spec.glyph);
      if (spec.id == "dnd" && services.notifications != nullptr) {
        action.subtitle = i18n::tr(
            services.notifications->doNotDisturb() ? "launcher.providers.actions.state-on"
                                                   : "launcher.providers.actions.state-off"
        );
      } else if (const auto open = openWindowFor(services.compositor, helixAppIds(spec.id)); open.has_value()) {
        action.subtitle = i18n::tr("launcher.providers.actions.switch-to", "name", open->title);
      } else if (spec.id == "workspace-next" || spec.id == "workspace-prev") {
        const std::string workspace = activeWorkspaceName(services.compositor);
        if (workspace.empty()) {
          action.subtitle = i18n::tr(spec.subtitleKey);
        } else {
          action.subtitle = i18n::tr(
              spec.id == "workspace-next" ? "launcher.providers.actions.workspace-next-from"
                                          : "launcher.providers.actions.workspace-prev-from",
              "name", workspace
          );
        }
      } else {
        action.subtitle = i18n::tr(spec.subtitleKey);
      }
      return action;
    }

  } // namespace

  std::string activeWorkspaceLabel(const CompositorPlatform* compositor) { return activeWorkspaceName(compositor); }

  bool closeWindow(CompositorPlatform* compositor, std::string_view windowId, std::string& error) {
    if (compositor == nullptr) {
      error = "no compositor";
      return false;
    }
    if (!StringUtils::trim(windowId).empty()) {
      ToplevelInfo info;
      info.identifier = std::string(StringUtils::trim(windowId));
      info.exactIdentity = true;
      compositor->closeToplevelInfo(info);
      return true;
    }
    if (!closeFocused(*compositor)) {
      error = "no focused window to close";
      return false;
    }
    return true;
  }

  std::string focusedWindowLabel(const CompositorPlatform* compositor) {
    if (compositor == nullptr) {
      return {};
    }
    if (const auto active = compositor->activeToplevel(); active.has_value()) {
      if (!active->title.empty()) {
        return active->title;
      }
      if (!active->appId.empty()) {
        return active->appId;
      }
    }
    auto focusedId = compositor->focusedCompositorWindowId();
    if (!focusedId.has_value() || focusedId->empty()) {
      focusedId = compositor->recentFocusedWindowId();
    }
    if (!focusedId.has_value() || focusedId->empty()) {
      return {};
    }
    for (const auto& window : compositor->workspaceWindowAssignments()) {
      if (window.windowId == *focusedId) {
        return window.title.empty() ? window.appId : window.title;
      }
    }
    return {};
  }

  std::vector<Action> query(std::string_view text, const Services& services) {
    std::vector<Action> actions;
    actions.reserve(std::size(kSpecs) + 1);
    if (const std::string focused = focusedWindowLabel(services.compositor); !focused.empty()) {
      Action close;
      close.id = "close-focused";
      close.title = i18n::tr("launcher.providers.actions.close-focused", "name", focused);
      close.subtitle = i18n::tr("launcher.providers.actions.close-focused-sub");
      close.glyph = "x";
      actions.push_back(std::move(close));
    }
    for (const auto& spec : kSpecs) {
      actions.push_back(makeAction(spec, services));
    }

    const std::string pattern = StringUtils::toLower(StringUtils::trim(text));
    if (pattern.empty()) {
      return actions;
    }

    std::vector<std::pair<double, Action>> scored;
    scored.reserve(actions.size());
    for (auto& action : actions) {
      const std::string searchable = StringUtils::toLower(action.title + " " + action.subtitle + " " + action.id);
      const double score = FuzzyMatch::score(pattern, searchable);
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
      scored.emplace_back(score, std::move(action));
    }
    std::ranges::sort(scored, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Action> matched;
    matched.reserve(scored.size());
    for (auto& entry : scored) {
      matched.push_back(std::move(entry.second));
    }
    return matched;
  }

  void schedule(const Services& services, Request request) {
    DeferredCall::callLater([services, request = std::move(request)]() {
      std::string error;
      if (!run(services, request, error) && !error.empty()) {
        kLog.warn("{}", error);
      }
    });
  }

  std::string launchQuery(
      CompositorPlatform* compositor, ConfigService* config, std::string_view query, std::string& error
  ) {
    const auto entry = resolveQuery(query);
    if (!entry.has_value()) {
      error = "no application matches '" + StringUtils::trim(query) + "'";
      return {};
    }
    if (!launchEntry(compositor, config, *entry, error)) {
      return {};
    }
    return entry->name.empty() ? entry->id : entry->name;
  }

} // namespace desktop_actions
