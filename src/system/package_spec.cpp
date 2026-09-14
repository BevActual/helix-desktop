#include "system/package_spec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace noctalia::packages {

  namespace {

    constexpr std::array kProtectedNames = {
        std::string_view{"helix-shell"},
        std::string_view{"noctalia"},
        std::string_view{"noctalia-greeter"},
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
