#include "desktop/display_inventory.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("helix-displays-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(root);
  const auto command = root / "niri";
  auto script = [&](const std::string& body) {
    std::ofstream(command) << "#!/bin/sh\n[ \"$*\" = \"msg --json outputs\" ] || exit 2\n" << body;
    std::filesystem::permissions(command, std::filesystem::perms::owner_all);
  };
  setenv("PATH", root.c_str(), 1);
  setenv("NIRI_SOCKET", "test-only-no-real-compositor", 1);
  auto inventory = std::make_shared<helix::DisplayInventory>();
  unsigned changes = 0;
  inventory->changed = [&] { ++changes; };
  auto wait = [&](auto condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      inventory->refresh(true);
      while (g_main_context_iteration(nullptr, false)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(condition());
  };
  const std::string valid =
      R"({"TEST-1":{"name":"TEST-1","make":"Test","model":"Display","serial":"not-exported","modes":[{"width":1920,"height":1080,"refresh_rate":60000,"is_preferred":true}],"current_mode":0,"vrr_supported":true,"vrr_enabled":false,"logical":{"x":0,"y":0,"width":1920,"height":1080,"scale":1.0,"transform":"Normal"}}})";
  script("printf '%s' '" + valid + "'\n");
  inventory->refresh(false);
  assert(!inventory->snapshot().at("available").get<bool>());
  wait([&] { return inventory->snapshot().at("available").get<bool>(); });
  auto state = inventory->snapshot();
  assert(state.at("outputs").size() == 1 && state.at("outputs").at(0).at("width") == 1920);
  assert(state.at("capabilities") == helix::Json::array({"inventory"}));
  assert(state.dump().find("not-exported") == std::string::npos);
  script("printf '%s' 'invalid-json'\n");
  wait([&] { return !inventory->snapshot().at("available").get<bool>(); });
  assert(inventory->snapshot().at("outputs").empty());
  script("printf '%s' '" + valid + "'\n");
  wait([&] { return inventory->snapshot().at("available").get<bool>(); });
  script("exec /usr/bin/sleep 30\n");
  wait([&] { return !inventory->snapshot().at("available").get<bool>(); });
  assert(inventory->snapshot().at("outputs").empty());
  inventory->refresh(false);
  assert(inventory->snapshot().at("capabilities").empty());
  assert(changes >= 5);
  std::filesystem::remove_all(root);
}
