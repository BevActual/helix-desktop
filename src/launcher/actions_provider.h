#pragma once

#include "launcher/desktop_actions.h"
#include "launcher/launcher_provider.h"

// Keyboard verbs for the agentic desktop: the same actions Caps Lock quick
// insert lists, reachable in the launcher as `act` and in global search.
class ActionsProvider : public LauncherProvider {
public:
  explicit ActionsProvider(desktop_actions::Services services);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "act"; }
  [[nodiscard]] std::string_view id() const override { return "Actions"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "bolt"; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

private:
  desktop_actions::Services m_services;
};
