#pragma once

#include "compositors/niri/niri_config_binds.h"

#include <filesystem>
#include <string>
#include <vector>

namespace compositors::niri {

  inline constexpr std::string_view kKeyboardIncludeFileName = "noctalia-keyboard.kdl";
  inline constexpr std::string_view kQuickInsertXkbOption = "helix:quickinsert";

  // Remap the Caps Lock key to F19 while Quick Insert owns it, so the physical
  // key does not toggle Caps. niri binds F19; Shift still types capitals.
  bool syncQuickInsertKeyboard(
      const std::filesystem::path& configFile, const std::vector<ParsedBind>& binds, std::string& error
  );

} // namespace compositors::niri
