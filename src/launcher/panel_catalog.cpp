#include "launcher/panel_catalog.h"

#include "i18n/i18n.h"

#include <algorithm>
#include <array>

namespace panel_catalog {

  namespace {

    constexpr std::string_view kFallbackGlyph = "apps";

    struct BuiltinPanelMeta {
      std::string_view id;
      std::string_view titleKey;
      std::string_view glyph;
    };

    // Every panel PanelManager registers for core (see application_ui.cpp). Core
    // panels ship no manifest to read a name/glyph from, so this is the only source
    // of truth for them; keep it in sync if a core panel id is added or renamed.
    constexpr std::array kBuiltinPanels = {
        BuiltinPanelMeta{"clipboard", "launcher.providers.panel.builtin.clipboard", "clipboard"},
        BuiltinPanelMeta{"control-center", "launcher.providers.panel.builtin.control-center", "adjustments"},
        BuiltinPanelMeta{"launcher", "launcher.providers.panel.builtin.launcher", "search"},
        BuiltinPanelMeta{"polkit", "launcher.providers.panel.builtin.polkit", "shield-lock"},
        BuiltinPanelMeta{"session", "launcher.providers.panel.builtin.session", "power"},
        BuiltinPanelMeta{"setup-wizard", "launcher.providers.panel.builtin.setup-wizard", "wand"},
        BuiltinPanelMeta{"test", "launcher.providers.panel.builtin.test", "bug"},
        BuiltinPanelMeta{"tray-drawer", "launcher.providers.panel.builtin.tray-drawer", "apps"},
        BuiltinPanelMeta{"wallpaper", "launcher.providers.panel.builtin.wallpaper", "wallpaper-selector"},
    };

  } // namespace

  Description describe(std::string_view panelId) {
    const auto builtin = std::ranges::find(kBuiltinPanels, panelId, &BuiltinPanelMeta::id);
    if (builtin != kBuiltinPanels.end()) {
      return Description{.title = i18n::tr(builtin->titleKey), .subtitle = {}, .glyph = std::string(builtin->glyph)};
    }

    return Description{.title = std::string(panelId), .subtitle = {}, .glyph = std::string(kFallbackGlyph)};
  }

  std::vector<std::string> allKnownIds() {
    std::vector<std::string> ids;
    ids.reserve(kBuiltinPanels.size());
    for (const auto& meta : kBuiltinPanels) {
      ids.emplace_back(meta.id);
    }
    return ids;
  }

} // namespace panel_catalog
