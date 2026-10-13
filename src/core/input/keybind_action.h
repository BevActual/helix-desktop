#pragma once

#include <cstdint>

// The actions UI controls can bind keys to. Defaults and config parsing live in config/.
enum class KeybindAction : std::uint8_t {
  Validate = 0,
  Cancel = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  TabNext = 6,
  TabPrevious = 7,
  Delete = 8,
  Copy = 9,
  Save = 10,
};
