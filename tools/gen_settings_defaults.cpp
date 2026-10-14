// Dumps the C++ settings catalog (ids, types, default values) for a pristine config, so the
// Rust bootstrap catalog can be generated and verified against the real defaults.
#include "config/config_service.h"
#include "desktop/settings_catalog.h"

#include <iostream>

int main() {
  ConfigService config;
  const auto catalog = helix::settingsCatalog(config, /*helixDesktop=*/true, /*gammaAvailable=*/true);
  std::cout << catalog.dump(1) << '\n';
}
