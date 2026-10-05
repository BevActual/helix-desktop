#include "system/package_spec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noctalia::packages {

  namespace {

    constexpr std::array kProtectedNames = {
        std::string_view{"helix-shell"},
        std::string_view{"noctalia"},
        std::string_view{"noctalia-greeter"},
        std::string_view{"helix-greeter"},
        std::string_view{"helix-niri"},
        std::string_view{"niri"},
        std::string_view{"filesystem"},
        std::string_view{"setup"},
        std::string_view{"kernel"},
        std::string_view{"kernel-core"},
        std::string_view{"kernel-modules"},
        std::string_view{"kernel-modules-core"},
        std::string_view{"kernel-modules-extra"},
        std::string_view{"glibc"},
        std::string_view{"systemd"},
        std::string_view{"systemd-libs"},
        std::string_view{"systemd-udev"},
        std::string_view{"rpm"},
        std::string_view{"dnf"},
        std::string_view{"dnf5"},

    };

    constexpr std::array kInfoLabels = {
        std::string_view{"Collection installed"},
        std::string_view{"Collection available"},
        std::string_view{"Bug fix"},
        std::string_view{"Enhancement"},
        std::string_view{"Unavailable"},
        std::string_view{"Available"},
        std::string_view{"Installed"},
        std::string_view{"Important"},
        std::string_view{"Security"},
        std::string_view{"Blocked"},
        std::string_view{"Normal"},
        std::string_view{"Low"},
    };

    std::string_view trim(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
      }
      return text;
    }

    bool isRpmNameChar(char ch, bool first) {
      if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
        return true;
      }
      if (first) {
        return false;
      }
      return ch == '.' || ch == '_' || ch == '+' || ch == '-';
    }

    bool parseNevra(std::string_view nevra, PackageHit& hit) {
      const auto lastDot = nevra.rfind('.');
      if (lastDot == std::string_view::npos || lastDot == 0 || lastDot + 1 >= nevra.size()) {
        return false;
      }
      hit.arch = std::string(nevra.substr(lastDot + 1));
      const auto rest = nevra.substr(0, lastDot);
      const auto colon = rest.find(':');
      if (colon != std::string_view::npos) {
        const auto dashBeforeEpoch = rest.rfind('-', colon);
        if (dashBeforeEpoch == std::string_view::npos || dashBeforeEpoch == 0) {
          return false;
        }
        hit.name = std::string(rest.substr(0, dashBeforeEpoch));
        hit.epoch = std::string(rest.substr(dashBeforeEpoch + 1, colon - dashBeforeEpoch - 1));
        const auto after = rest.substr(colon + 1);
        const auto dash = after.rfind('-');
        if (dash == std::string_view::npos || dash == 0 || dash + 1 >= after.size()) {
          return false;
        }
        hit.version = std::string(after.substr(0, dash));
        hit.release = std::string(after.substr(dash + 1));
      } else {
        const auto dashRel = rest.rfind('-');
        if (dashRel == std::string_view::npos || dashRel == 0 || dashRel + 1 >= rest.size()) {
          return false;
        }
        hit.release = std::string(rest.substr(dashRel + 1));
        const auto nameVer = rest.substr(0, dashRel);
        const auto dashVer = nameVer.rfind('-');
        if (dashVer == std::string_view::npos || dashVer == 0 || dashVer + 1 >= nameVer.size()) {
          return false;
        }
        hit.name = std::string(nameVer.substr(0, dashVer));
        hit.version = std::string(nameVer.substr(dashVer + 1));
      }
      return isValidRpmName(hit.name) && !hit.version.empty() && !hit.release.empty() && !hit.arch.empty();
    }

  } // namespace

  bool isValidRpmName(std::string_view name) {
    if (name.empty() || name.size() > 256) {
      return false;
    }
    if (!isRpmNameChar(name.front(), true)) {
      return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
      if (!isRpmNameChar(name[i], false)) {
        return false;
      }
    }
    return true;
  }

  bool isValidFlatpakAppId(std::string_view id) {
    if (id.size() < 3 || id.size() > 255) {
      return false;
    }
    if (id.contains('/') || id.contains('\\') || id.contains("..")) {
      return false;
    }
    bool start = true;
    std::size_t dots = 0;
    for (const char ch : id) {
      if (start) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
          return false;
        }
        start = false;
        continue;
      }
      if (ch == '.') {
        ++dots;
        start = true;
        continue;
      }
      if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_' && ch != '-') {
        return false;
      }
    }
    return dots >= 1 && !start;
  }

  bool isValidFwupdDeviceId(std::string_view id) {
    if (id.size() < 8 || id.size() > 64) {
      return false;
    }
    for (const char ch : id) {
      const auto uchar = static_cast<unsigned char>(ch);
      if (std::isxdigit(uchar) == 0) {
        return false;
      }
    }
    return true;
  }

  std::string preferredFlatpakRemote(std::string_view remotes) {
    std::string best;
    std::size_t start = 0;
    while (start <= remotes.size()) {
      const auto end = remotes.find(',', start);
      auto token = remotes.substr(start, end == std::string_view::npos ? remotes.size() - start : end - start);
      while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())) != 0) {
        token.remove_prefix(1);
      }
      while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
        token.remove_suffix(1);
      }
      if (!token.empty() && best.empty()) {
        best = std::string(token);
      }
      if (token == "flathub") {
        return "flathub";
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return best;
  }

  bool hasSafeSearchChars(std::string_view query) {
    for (const char ch : query) {
      if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
        continue;
      }
      if (ch == ' ' || ch == '.' || ch == '_' || ch == '+' || ch == '-') {
        continue;
      }
      return false;
    }
    return true;
  }

  bool isSafeSearchQuery(std::string_view query) {
    const auto trimmed = trim(query);
    return trimmed.size() >= 2 && trimmed.size() <= 80 && hasSafeSearchChars(trimmed);
  }

  bool isProtectedPackage(std::string_view name) {
    return std::ranges::find(kProtectedNames, name) != kProtectedNames.end();
  }

  bool applyPkconProgressLine(std::string_view line, PkconProgress& progress) {
    line = trim(line);
    if (line.starts_with("Status:")) {
      progress.status = std::string(trim(line.substr(7)));
      return !progress.status.empty();
    }
    if (line.starts_with("Percentage:")) {
      const auto rest = trim(line.substr(11));
      if (rest.empty() || std::isdigit(static_cast<unsigned char>(rest.front())) == 0) {
        return false;
      }
      int value = 0;
      for (const char ch : rest) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
          break;
        }
        value = value * 10 + (ch - '0');
        if (value > 100) {
          value = 100;
          break;
        }
      }
      progress.percent = value;
      return true;
    }
    return false;
  }

  PackageState stateFromInfo(std::string_view info) {
    if (info == "Installed" || info == "Collection installed") {
      return PackageState::Installed;
    }
    if (info == "Available" || info == "Collection available" || info == "Unavailable") {
      return PackageState::Available;
    }
    if (info == "Bug fix" || info == "Enhancement" || info == "Security" || info == "Important" || info == "Normal"
        || info == "Low") {
      return PackageState::Update;
    }
    return PackageState::Other;
  }

  std::optional<PackageHit> parsePkconLine(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.starts_with("Transaction:") || line.starts_with("Status:") || line.starts_with("Results:")) {
      return std::nullopt;
    }

    std::string_view repo;
    std::string_view body = line;
    if (line.back() == ')') {
      const auto open = line.rfind(" (");
      if (open != std::string_view::npos && open + 2 < line.size()) {
        repo = line.substr(open + 2, line.size() - open - 3);
        body = trim(line.substr(0, open));
      }
    }

    std::string_view info;
    for (const auto label : kInfoLabels) {
      if (body.size() > label.size() && body.substr(0, label.size()) == label
          && std::isspace(static_cast<unsigned char>(body[label.size()])) != 0) {
        info = label;
        body = trim(body.substr(label.size()));
        break;
      }
    }
    if (info.empty() || body.empty()) {
      return std::nullopt;
    }

    PackageHit hit;
    hit.info = std::string(info);
    hit.repo = std::string(repo);
    hit.state = stateFromInfo(info);
    if (!parseNevra(body, hit)) {
      return std::nullopt;
    }
    return hit;
  }

  std::vector<PackageHit> parsePkconOutput(std::string_view text) {
    std::vector<PackageHit> hits;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      const auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      if (auto hit = parsePkconLine(line)) {
        hits.push_back(std::move(*hit));
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return hits;
  }

  std::optional<PackageHit> parseRpmInstalledLine(std::string_view line) {
    line = trim(line);
    if (line.empty()) {
      return std::nullopt;
    }

    std::array<std::string_view, 5> fields{};
    std::size_t field = 0;
    std::size_t start = 0;
    while (field < fields.size()) {
      const auto end = line.find('\t', start);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      fields[field] = line.substr(start, end - start);
      start = end + 1;
      ++field;
    }
    const auto summary = line.substr(start);

    PackageHit hit;
    hit.name = std::string(trim(fields[0]));
    const auto epoch = trim(fields[1]);
    if (!epoch.empty() && epoch != "0" && epoch != "(none)") {
      hit.epoch = std::string(epoch);
    }
    hit.version = std::string(trim(fields[2]));
    hit.release = std::string(trim(fields[3]));
    hit.arch = std::string(trim(fields[4]));
    hit.summary = compactSummary(summary);
    hit.info = "Installed";
    hit.state = PackageState::Installed;
    if (!isValidRpmName(hit.name) || hit.version.empty() || hit.release.empty() || hit.arch.empty()) {
      return std::nullopt;
    }
    return hit;
  }

  std::vector<PackageHit> parseRpmInstalledOutput(std::string_view text) {
    std::vector<PackageHit> hits;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      const auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      if (auto hit = parseRpmInstalledLine(line)) {
        hits.push_back(std::move(*hit));
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return hits;
  }

  std::optional<PackageHit> parseFlatpakListLine(std::string_view line, FlatpakListKind kind) {
    line = trim(line);
    if (line.empty()) {
      return std::nullopt;
    }
    const std::size_t expected = kind == FlatpakListKind::Installed ? 6 : 5;
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (fields.size() + 1 < expected) {
      const auto end = line.find('\t', start);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      fields.push_back(line.substr(start, end - start));
      start = end + 1;
    }
    fields.push_back(line.substr(start));
    if (fields.size() != expected) {
      return std::nullopt;
    }

    PackageHit hit;
    hit.name = std::string(trim(fields[0]));
    if (!isValidFlatpakAppId(hit.name)) {
      return std::nullopt;
    }
    hit.title = compactSummary(fields[1]);
    if (kind == FlatpakListKind::Installed) {
      hit.version = std::string(trim(fields[2]));
      hit.release = std::string(trim(fields[3]));
      hit.repo = std::string(trim(fields[4]));
      hit.installation = std::string(trim(fields[5]));
      hit.info = "Installed";
      hit.state = PackageState::Installed;
      if (!hit.title.empty()) {
        hit.summary = hit.title;
      }
    } else {
      hit.summary = compactSummary(fields[2]);
      hit.version = std::string(trim(fields[3]));
      hit.repo = std::string(trim(fields[4]));
      hit.info = "Update";
      hit.state = PackageState::Update;
    }
    return hit;
  }

  std::vector<PackageHit> parseFlatpakListOutput(std::string_view text, FlatpakListKind kind) {
    std::vector<PackageHit> hits;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      const auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      if (auto hit = parseFlatpakListLine(line, kind)) {
        hits.push_back(std::move(*hit));
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return hits;
  }

  std::vector<PackageHit> parseFlatpakSearchJson(std::string_view text) {
    std::vector<PackageHit> hits;
    auto jsonText = std::string(text);
    if (const auto start = jsonText.find('['); start != std::string::npos && start > 0) {
      jsonText.erase(0, start);
    }
    const auto json = nlohmann::json::parse(jsonText, nullptr, false);
    if (!json.is_array()) {
      return hits;
    }
    for (const auto& row : json) {
      if (!row.is_object()) {
        continue;
      }
      PackageHit hit;
      if (row.contains("application_id") && row["application_id"].is_string()) {
        hit.name = row["application_id"].get<std::string>();
      } else if (row.contains("application") && row["application"].is_string()) {
        hit.name = row["application"].get<std::string>();
      }
      if (!isValidFlatpakAppId(hit.name)) {
        continue;
      }
      if (row.contains("name") && row["name"].is_string()) {
        hit.title = compactSummary(row["name"].get<std::string>());
      }
      if (row.contains("description") && row["description"].is_string()) {
        hit.summary = compactSummary(row["description"].get<std::string>());
      }
      if (row.contains("version") && row["version"].is_string()) {
        hit.version = row["version"].get<std::string>();
      }
      if (row.contains("remotes") && row["remotes"].is_string()) {
        hit.repo = row["remotes"].get<std::string>();
      }
      hit.info = "Available";
      hit.state = PackageState::Available;
      hits.push_back(std::move(hit));
    }
    return hits;
  }

  namespace {

    std::string stripHtml(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      bool inTag = false;
      for (const char ch : text) {
        if (ch == '<') {
          inTag = true;
          continue;
        }
        if (ch == '>') {
          inTag = false;
          out.push_back(' ');
          continue;
        }
        if (!inTag) {
          out.push_back(ch);
        }
      }
      return out;
    }

    bool deviceHasFlag(const nlohmann::json& device, std::string_view flag) {
      if (!device.contains("Flags") || !device["Flags"].is_array()) {
        return false;
      }
      for (const auto& entry : device["Flags"]) {
        if (entry.is_string() && entry.get<std::string>() == flag) {
          return true;
        }
      }
      return false;
    }

    std::string jsonString(const nlohmann::json& object, const char* key) {
      if (!object.contains(key) || !object[key].is_string()) {
        return {};
      }
      return object[key].get<std::string>();
    }

  } // namespace

  std::vector<PackageHit> parseFwupdDevicesJson(std::string_view text, FwupdListKind kind) {
    std::vector<PackageHit> hits;
    auto jsonText = std::string(text);
    if (const auto start = jsonText.find('{'); start != std::string::npos && start > 0) {
      jsonText.erase(0, start);
    }
    const auto json = nlohmann::json::parse(jsonText, nullptr, false);
    if (!json.is_object() || !json.contains("Devices") || !json["Devices"].is_array()) {
      return hits;
    }
    for (const auto& device : json["Devices"]) {
      if (!device.is_object()) {
        continue;
      }
      PackageHit hit;
      hit.name = jsonString(device, "DeviceId");
      if (!isValidFwupdDeviceId(hit.name)) {
        continue;
      }
      if (kind == FwupdListKind::Installed) {
        if (!deviceHasFlag(device, "updatable")) {
          continue;
        }
        hit.version = jsonString(device, "Version");
        if (hit.version.empty()) {
          continue;
        }
        hit.title = compactSummary(jsonString(device, "Name"));
        hit.summary = compactSummary(jsonString(device, "Summary"));
        hit.repo = jsonString(device, "Vendor");
        hit.info = "Installed";
        hit.state = PackageState::Installed;
      } else {
        if (!device.contains("Releases") || !device["Releases"].is_array() || device["Releases"].empty()) {
          continue;
        }
        const auto& release = device["Releases"].front();
        if (!release.is_object()) {
          continue;
        }
        hit.title = compactSummary(jsonString(device, "Name"));
        hit.version = jsonString(release, "Version");
        if (hit.version.empty()) {
          hit.version = jsonString(device, "Version");
        }
        hit.release = jsonString(device, "Version");
        std::string summary = jsonString(release, "Summary");
        if (summary.empty()) {
          summary = jsonString(device, "Summary");
        }
        if (summary.empty()) {
          summary = stripHtml(jsonString(release, "Description"));
        }
        hit.summary = compactSummary(summary);
        hit.repo = jsonString(release, "RemoteId");
        if (hit.repo.empty()) {
          hit.repo = jsonString(device, "Vendor");
        }
        hit.info = "Update";
        hit.state = PackageState::Update;
      }
      hits.push_back(std::move(hit));
    }
    return hits;
  }

  std::string compactSummary(std::string_view text) {
    std::string out;
    out.reserve(std::min(text.size(), std::size_t{240}));
    bool pendingSpace = false;
    for (const char ch : text) {
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        pendingSpace = true;
        continue;
      }
      if (pendingSpace && !out.empty()) {
        out.push_back(' ');
      }
      pendingSpace = false;
      out.push_back(ch);
      if (out.size() >= 240) {
        break;
      }
    }
    return out;
  }

  std::optional<std::pair<std::string, std::string>> parseNameSummaryLine(std::string_view line, char separator) {
    line = trim(line);
    if (line.empty()) {
      return std::nullopt;
    }
    const auto split = line.find(separator);
    if (split == std::string_view::npos || split == 0 || split + 1 >= line.size()) {
      return std::nullopt;
    }
    const auto name = trim(line.substr(0, split));
    const auto summary = compactSummary(line.substr(split + 1));
    if (!isValidRpmName(name) || summary.empty()) {
      return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::string(name), std::move(summary)};
  }

  std::unordered_map<std::string, std::string> parseNameSummaryOutput(std::string_view text, char separator) {
    std::unordered_map<std::string, std::string> summaries;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      const auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      if (auto row = parseNameSummaryLine(line, separator)) {
        summaries.emplace(std::move(row->first), std::move(row->second));
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return summaries;
  }

  void applySummaryMap(std::vector<PackageHit>& hits, const std::unordered_map<std::string, std::string>& summaries) {
    for (auto& hit : hits) {
      if (!hit.summary.empty()) {
        continue;
      }
      const auto it = summaries.find(hit.name);
      if (it != summaries.end()) {
        hit.summary = it->second;
      }
    }
  }

  std::vector<PackageHit> dedupeByName(std::vector<PackageHit> hits) {
    std::unordered_map<std::string, std::size_t> indexByName;
    std::vector<PackageHit> out;
    out.reserve(hits.size());
    for (auto& hit : hits) {
      const auto it = indexByName.find(hit.name);
      if (it == indexByName.end()) {
        indexByName.emplace(hit.name, out.size());
        out.push_back(std::move(hit));
        continue;
      }
      auto& existing = out[it->second];
      const auto rank = [](PackageState state) {
        switch (state) {
        case PackageState::Update:
          return 3;
        case PackageState::Installed:
          return 2;
        case PackageState::Available:
          return 1;
        case PackageState::Other:
          return 0;
        }
        return 0;
      };
      if (rank(hit.state) > rank(existing.state)) {
        existing = std::move(hit);
      }
    }
    return out;
  }

  void sortHits(std::vector<PackageHit>& hits) {
    const auto stateRank = [](PackageState state) {
      switch (state) {
      case PackageState::Update:
        return 3;
      case PackageState::Installed:
        return 2;
      case PackageState::Available:
        return 1;
      case PackageState::Other:
        return 0;
      }
      return 0;
    };
    const auto infoRank = [](std::string_view info) {
      if (info == "Security") {
        return 6;
      }
      if (info == "Important") {
        return 5;
      }
      if (info == "Bug fix") {
        return 4;
      }
      if (info == "Enhancement") {
        return 3;
      }
      if (info == "Normal") {
        return 2;
      }
      if (info == "Low") {
        return 1;
      }
      return 0;
    };
    std::ranges::sort(hits, [&](const PackageHit& left, const PackageHit& right) {
      const int leftState = stateRank(left.state);
      const int rightState = stateRank(right.state);
      if (leftState != rightState) {
        return leftState > rightState;
      }
      const int leftInfo = infoRank(left.info);
      const int rightInfo = infoRank(right.info);
      if (leftInfo != rightInfo) {
        return leftInfo > rightInfo;
      }
      return std::lexicographical_compare(
          left.name.begin(), left.name.end(), right.name.begin(), right.name.end(),
          [](char leftCh, char rightCh) {
            return std::tolower(static_cast<unsigned char>(leftCh)) < std::tolower(static_cast<unsigned char>(rightCh));
          }
      );
    });
  }

} // namespace noctalia::packages
