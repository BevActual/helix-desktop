#include "inference/routing.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace helix::inference {
namespace {

namespace fs = std::filesystem;

fs::path stateDir() {
  if (const char* helix = std::getenv("HELIX_STATE_HOME"); helix && *helix)
    return fs::path(helix) / "helix";
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) return fs::path(xdg) / "helix";
  if (const char* home = std::getenv("HOME"); home && *home)
    return fs::path(home) / ".local/state/helix";
  return fs::path("helix");
}

void ensureDefaults(RoutingTable& out) {
  if (out.backends.empty()) out.backends.push_back({"local", "local", "", "On this computer"});
  for (const char* task : {"act", "chat", "summarize", "vision", "code"})
    if (!out.profiles.contains(task)) out.profiles[task] = ProfileRoute{"local", ""};
}

void upsertBackend(RoutingTable& out, BackendInfo backend) {
  if (backend.id.empty()) return;
  for (auto& existing : out.backends) {
    if (existing.id == backend.id) {
      existing = std::move(backend);
      return;
    }
  }
  out.backends.push_back(std::move(backend));
}

// Minimal TOML subset for inference-routing.toml (tables and string/bool assignments).
void parseRouting(const std::string& text, RoutingTable& out) {
  out = RoutingTable{};
  ensureDefaults(out);

  std::string section;
  std::string profileName;
  BackendInfo currentBackend;
  bool backendOpen = false;

  auto flushBackend = [&] {
    if (backendOpen) upsertBackend(out, currentBackend);
    currentBackend = BackendInfo{};
    backendOpen = false;
  };

  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    auto start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') continue;
    line = line.substr(start);

    if (line.starts_with("[[") && line.size() >= 4) {
      const auto end = line.rfind("]]");
      if (end == std::string::npos) continue;
      flushBackend();
      section = line.substr(2, end - 2);
      profileName.clear();
      if (section == "backends") {
        currentBackend = BackendInfo{};
        backendOpen = true;
      }
      continue;
    }

    if (line.starts_with('[')) {
      const auto end = line.find(']');
      if (end == std::string::npos) continue;
      flushBackend();
      section = line.substr(1, end - 1);
      profileName.clear();
      if (section.starts_with("profiles.")) {
        profileName = section.substr(9);
        out.profiles[profileName];
      }
      continue;
    }

    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto key = line.substr(0, eq);
    auto value = line.substr(eq + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);

    if (!profileName.empty()) {
      auto& route = out.profiles[profileName];
      if (key == "backend")
        route.backend = value;
      else if (key == "model")
        route.model = value;
    } else if (backendOpen) {
      if (key == "id")
        currentBackend.id = value;
      else if (key == "kind")
        currentBackend.kind = value;
      else if (key == "base_url")
        currentBackend.baseUrl = value;
      else if (key == "label")
        currentBackend.label = value;
    } else if (key == "allow_remote_on_battery_saver") {
      out.allowRemoteOnBatterySaver = (value == "true");
    }
  }
  flushBackend();
  ensureDefaults(out);
}

}  // namespace

ProfileRoute RoutingTable::route(const std::string& task) const {
  if (const auto it = profiles.find(task); it != profiles.end()) return it->second;
  return ProfileRoute{"local", ""};
}

bool RoutingTable::isLocal(const std::string& task) const {
  const auto r = route(task);
  if (r.backend.empty() || r.backend == "local") return true;
  for (const auto& b : backends)
    if (b.id == r.backend) return b.kind == "local";
  return false;
}

RoutingTable loadRouting() {
  RoutingTable table;
  const fs::path path = stateDir() / "inference-routing.toml";
  std::ifstream in(path);
  if (!in) {
    ensureDefaults(table);
    return table;
  }
  std::ostringstream body;
  body << in.rdbuf();
  parseRouting(body.str(), table);
  return table;
}

}  // namespace helix::inference
