#include "shell/control_center/tabs/native_redirect_tab.h"

#include "compositors/compositor_platform.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/flex.h"

#include <utility>

NativeRedirectTab::NativeRedirectTab(
    std::string application, std::string page, std::string titleKey, CompositorPlatform* platform
)
    : m_application(std::move(application)), m_page(std::move(page)), m_titleKey(std::move(titleKey)),
      m_platform(platform) {}

bool NativeRedirectTab::launch(std::string_view application, std::string_view page, CompositorPlatform* platform) {
  const bool monitor = application == "helix-system-monitor";
  DesktopEntry entry;
  entry.id = monitor ? "org.helix.SystemMonitor" : "org.helix.Settings";
  entry.name = monitor ? "Helix System Monitor" : "Helix Settings";
  entry.exec = std::string(application) + " --page " + std::string(page);
  desktop_entry_launch::LaunchOptions options;
  if (platform != nullptr && platform->hasXdgActivation()) {
    options.activationToken = platform->requestActivationToken(nullptr);
  }
  if (!desktop_entry_launch::launchEntry(entry, options)) {
    return false;
  }
  PanelManager::instance().close();
  return true;
}

std::unique_ptr<Flex> NativeRedirectTab::create() {
  const bool monitor = m_application == "helix-system-monitor";
  const bool available = process::commandExists(m_application.c_str());
  const std::string label = i18n::tr(monitor ? "control-center.open-system-monitor" : "control-center.open-settings");

  control_center::EmptyStateSpec spec{
      .glyph = monitor ? "activity-heartbeat" : "settings",
      .title = i18n::tr(m_titleKey),
      .body = available ? label
                        : i18n::tr(monitor ? "control-center.install-system-monitor" : "control-center.install-settings"),
      .actionLabel = available ? label : std::string{},
      .actionGlyph = monitor ? "activity-heartbeat" : "settings",
      .onAction = [application = m_application, page = m_page, platform = m_platform]() {
        (void)NativeRedirectTab::launch(application, page, platform);
      },
  };
  auto root = control_center::makeEmptyState(contentScale(), spec);
  root->setFlexGrow(1.0F);
  return root;
}
