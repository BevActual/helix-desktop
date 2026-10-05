#pragma once

#include "shell/control_center/tab.h"

#include <memory>
#include <string>
#include <string_view>

class CompositorPlatform;
class Flex;

// Placeholder for a tab whose live page now lives in a standalone Helix application.
// Shows where the feature moved and a single button that opens it.
class NativeRedirectTab : public Tab {
public:
  NativeRedirectTab(std::string application, std::string page, std::string titleKey, CompositorPlatform* platform);

  std::unique_ptr<Flex> create() override;

  // Launches `application --page <page>` and closes the Control Center on success.
  static bool launch(std::string_view application, std::string_view page, CompositorPlatform* platform);

private:
  std::string m_application;
  std::string m_page;
  std::string m_titleKey;
  CompositorPlatform* m_platform;
};
