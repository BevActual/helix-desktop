#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace helix::inference {

struct BackendInfo {
  std::string id;
  std::string kind;  // local | openai
  std::string baseUrl;
  std::string label;
};

struct ProfileRoute {
  std::string backend{"local"};
  std::string model;
};

// Loads ~/.local/state/helix/inference-routing.toml (or HELIX_STATE_HOME/helix/…).
// Missing or invalid file → defaults (all profiles local, empty model).
struct RoutingTable {
  std::vector<BackendInfo> backends;
  std::unordered_map<std::string, ProfileRoute> profiles;
  bool allowRemoteOnBatterySaver = false;

  ProfileRoute route(const std::string& task) const;
  bool isLocal(const std::string& task) const;
};

RoutingTable loadRouting();

}  // namespace helix::inference
