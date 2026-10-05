#pragma once
#include "desktop/protocol.h"
#include "config/config_service.h"

namespace helix {
Json settingsCatalog(ConfigService& config, bool helixDesktop, bool gammaAvailable = false);
std::vector<std::string> settingPath(std::string_view id);
bool isPortableSetting(std::string_view id);
ConfigOverrideValue settingValue(const Json& value, std::string_view id = {});
void validateSettingValue(ConfigService& config, std::string_view id, const Json& value, bool desktop, bool gammaAvailable = false);
}
