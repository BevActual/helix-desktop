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
  DesktopEntry entry;
  if (application == "helix-system-monitor") {
    entry.id = "org.helix.SystemMonitor";
    entry.name = "Helix System Monitor";
  } else if (application == "helix-phone") {
    entry.id = "org.helix.Phone";
    entry.name = "Helix Phone";
  } else {
    entry.id = "org.helix.Settings";
    entry.name = "Helix Settings";
  }
  entry.exec = std::string(application);
  if (application != "helix-phone") {
    entry.exec += " --page " + std::string(page);
  }
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
  const bool phone = m_application == "helix-phone";
  const bool available = process::commandExists(m_application.c_str());
  const char* openKey = monitor ? "control-center.open-system-monitor"
      : phone                   ? "control-center.open-phone"
                                : "control-center.open-settings";
  const char* installKey = monitor ? "control-center.install-system-monitor"
      : phone                      ? "control-center.install-phone"
                                   : "control-center.install-settings";
  const std::string label = i18n::tr(openKey);

  control_center::EmptyStateSpec spec{
      .glyph = monitor ? "activity-heartbeat" : phone ? "device-mobile" : "settings",
      .title = i18n::tr(m_titleKey),
      .body = available ? label : i18n::tr(installKey),
      .actionLabel = available ? label : std::string{},
      .actionGlyph = monitor ? "activity-heartbeat" : phone ? "device-mobile" : "settings",
      .onAction = [application = m_application, page = m_page, platform = m_platform]() {
        (void)NativeRedirectTab::launch(application, page, platform);
      },
  };
  auto root = control_center::makeEmptyState(contentScale(), spec);
  root->setFlexGrow(1.0F);
  return root;
}
