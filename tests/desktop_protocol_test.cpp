#include "desktop_api/protocol.h"

#include <cassert>
#include <stdexcept>

int main() {
  const helix::Json expected = {
      {"available", true},
      {"volume", 0.5},
      {"id", std::uint64_t{42}},
      {"signal", std::int64_t{-20}},
      {"name", "Device <name>"},
      {"devices", {{{"name", "Phone"}, {"paired", false}}}},
      {"empty", helix::Json::array()},
      {"capabilities", helix::Json::object()}
  };
  auto* value = g_variant_ref_sink(helix::toVariant(expected));
  assert(g_variant_is_of_type(value, G_VARIANT_TYPE_VARDICT));
  assert(helix::fromVariant(value) == expected);
  g_variant_unref(value);
  assert(helix::interfaceFor("displays") == helix::displaysInterface);
  bool rejected = false;
  try {
    (void)helix::toVariant(nullptr);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
  rejected = false;
  try {
    (void)helix::interfaceFor("unknown");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
  return 0;
}
