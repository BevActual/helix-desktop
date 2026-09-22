#pragma once

#include "shell/settings/widget_settings_registry.h"

#include <string>
#include <vector>

namespace settings {

  // Installed font families, deduplicated and sorted case-insensitively. Discovered once via
  // `fc-list` and cached for the process lifetime.
  [[nodiscard]] const std::vector<std::string>& discoverFontFamilies();

  // Runs the `fc-list` discovery on a worker so the first Type tab open does not pay for it on
  // the UI thread. Safe to call once at startup; a concurrent discoverFontFamilies() simply waits
  // for the same static initialisation.
  void prewarmFontFamilies();

  // Select options for a per-widget font picker: an inherit/default entry (empty value) followed by
  // every installed family. Option labels are literal font names (not i18n keys), so the owning spec
  // must set `literalLabels = true`.
  [[nodiscard]] std::vector<WidgetSettingSelectOption> buildFontFamilySelectOptions();

} // namespace settings
