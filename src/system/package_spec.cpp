#include "system/package_spec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix::packages {

  namespace {

    constexpr std::array kProtectedNames = {
        std::string_view{"helix-shell"},
        std::string_view{"helix"},
        std::string_view{"helix-greeter"},
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
        std::string_view{"bash"},
        std::string_view{"coreutils"},
        std::string_view{"util-linux"},
        std::string_view{"shadow-utils"},
        std::string_view{"pam"},
        std::string_view{"selinux-policy"},
        std::string_view{"selinux-policy-targeted"},
        std::string_view{"grub2-common"},
        std::string_view{"grub2-tools"},
        std::string_view{"NetworkManager"},
        std::string_view{"mesa-dri-drivers"},

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

  namespace {

    bool isPkconIdFieldChar(char ch) {
      // RPM/NEVRA versioning allows ~ and ^; PackageKit data may include ':'.
      return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '_' || ch == '+' || ch == '-'
          || ch == ':' || ch == '~' || ch == '^';
    }

    bool isValidPkconIdField(std::string_view field, std::size_t maxLen = 128) {
      if (field.empty() || field.size() > maxLen) {
        return false;
      }
      for (const char ch : field) {
        if (!isPkconIdFieldChar(ch)) {
          return false;
        }
      }
      return true;
    }

  } // namespace

  bool isValidPkconPackageId(std::string_view id) {
    if (id.empty() || id.size() > 512) {
      return false;
    }
    const auto first = id.find(';');
    if (first == std::string_view::npos || first == 0) {
      return false;
    }
    const auto second = id.find(';', first + 1);
    if (second == std::string_view::npos || second == first + 1) {
      return false;
    }
    const auto third = id.find(';', second + 1);
    if (third == std::string_view::npos || third == second + 1 || third + 1 >= id.size()) {
      return false;
    }
    if (id.find(';', third + 1) != std::string_view::npos) {
      return false;
    }
    return isValidRpmName(id.substr(0, first)) && isValidPkconIdField(id.substr(first + 1, second - first - 1))
        && isValidPkconIdField(id.substr(second + 1, third - second - 1), 64)
        && isValidPkconIdField(id.substr(third + 1), 256);
  }

  bool isValidPkconPackageRef(std::string_view ref) {
    return isValidPkconPackageId(ref) || isValidRpmName(ref);
  }

  std::string pkconPackageId(const PackageHit& hit) {
    if (!isValidRpmName(hit.name) || !isValidPkconIdField(hit.version) || !isValidPkconIdField(hit.release)
        || !isValidPkconIdField(hit.arch, 64)) {
      return {};
    }
    if (!hit.epoch.empty() && !isValidPkconIdField(hit.epoch, 32)) {
      return {};
    }
    if (!hit.repo.empty() && !isValidPkconIdField(hit.repo, 256)) {
      return {};
    }
    std::string version;
    if (!hit.epoch.empty() && hit.epoch != "0") {
      version = hit.epoch;
      version.push_back(':');
    }
    version += hit.version;
    version.push_back('-');
    version += hit.release;
    const auto data = hit.repo.empty() ? "installed" : hit.repo;
    return hit.name + ';' + version + ';' + hit.arch + ';' + data;
  }

  std::string pkconPackageRef(const PackageHit& hit) {
    if (auto id = pkconPackageId(hit); !id.empty()) {
      return id;
    }
    return isValidRpmName(hit.name) ? hit.name : std::string{};
  }

  std::string pkconPackageRefForAction(const PackageHit& hit, bool removing) {
    auto packageRef = pkconPackageRef(hit);
    if (!removing || !isValidPkconPackageId(packageRef)) {
      return packageRef;
    }
    const auto sep = packageRef.rfind(';');
    if (sep == std::string::npos || sep + 1 >= packageRef.size()) {
      return packageRef;
    }
    const auto data = std::string_view(packageRef).substr(sep + 1);
    if (data.starts_with("installed")) {
      return packageRef;
    }
    return packageRef.substr(0, sep + 1) + "installed:" + std::string(data);
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

  bool isValidFlatpakRemoteName(std::string_view name) {
    if (name.empty() || name.size() > 128) {
      return false;
    }
    if (!std::isalnum(static_cast<unsigned char>(name.front()))) {
      return false;
    }
    for (const char ch : name) {
      if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '-' && ch != '_' && ch != '.') {
        return false;
      }
    }
    return true;
  }

  std::optional<FlatpakRemote> parseFlatpakRemoteLine(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.starts_with("Name")) {
      return std::nullopt;
    }
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
      const auto end = line.find('\t', start);
      fields.push_back(line.substr(start, end == std::string_view::npos ? line.size() - start : end - start));
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    if (fields.size() < 4) {
      return std::nullopt;
    }
    FlatpakRemote remote;
    remote.name = std::string(trim(fields[0]));
    if (!isValidFlatpakRemoteName(remote.name)) {
      return std::nullopt;
    }
    remote.title = std::string(trim(fields[1]));
    remote.url = std::string(trim(fields[2]));
    const auto options = trim(fields[3]);
    remote.installation = "system";
    std::size_t optStart = 0;
    while (optStart <= options.size()) {
      const auto optEnd = options.find(',', optStart);
      auto token = options.substr(
          optStart, optEnd == std::string_view::npos ? options.size() - optStart : optEnd - optStart
      );
      token = trim(token);
      if (token == "user") {
        remote.installation = "user";
      } else if (token == "disabled") {
        remote.disabled = true;
      } else if (token == "filtered") {
        remote.filtered = true;
      } else if (token == "oci") {
        remote.oci = true;
      }
      if (optEnd == std::string_view::npos) {
        break;
      }
      optStart = optEnd + 1;
    }
    if (fields.size() >= 5) {
      try {
        remote.priority = std::stoi(std::string(trim(fields[4])));
      } catch (...) {
        remote.priority = 1;
      }
    }
    if (remote.title.empty()) {
      remote.title = remote.name;
    }
    return remote;
  }

  std::vector<FlatpakRemote> parseFlatpakRemotesOutput(std::string_view text) {
    std::vector<FlatpakRemote> remotes;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      const auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      if (auto remote = parseFlatpakRemoteLine(line)) {
        remotes.push_back(std::move(*remote));
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return remotes;
  }

  bool hasUserFlathubRemote(const std::vector<FlatpakRemote>& remotes) {
    for (const auto& remote : remotes) {
      if (remote.name == "flathub" && remote.installation == "user" && !remote.disabled) {
        return true;
      }
    }
    return false;
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

  bool isRpmDevelPackage(std::string_view name) {
    return name.ends_with("-devel") || name.ends_with("-debuginfo") || name.ends_with("-debugsource")
        || name.ends_with("-static") || name.ends_with("-headers") || name.ends_with("-javadoc");
  }

  bool isRpmLibraryPackage(std::string_view name) {
    if (isRpmDevelPackage(name)) {
      return false;
    }
    if (name.ends_with("-libs") || name.ends_with("-lib") || name.ends_with("-common")
        || name.ends_with("-data") || name.ends_with("-locale") || name.ends_with("-langpacks")
        || name.ends_with("-filesystem")) {
      return true;
    }
    // libfoo packages are usually shared libraries; keep well-known app names out.
    if (name.starts_with("lib") && name != "libreoffice" && !name.starts_with("libreoffice-")
        && name != "librewolf" && !name.starts_with("library")) {
      return true;
    }
    return false;
  }

  bool isRpmAppPackage(std::string_view name) {
    return !name.empty() && !isRpmDevelPackage(name) && !isRpmLibraryPackage(name);
  }

  bool matchesRpmScope(std::string_view name, std::string_view scope) {
    if (scope.empty() || scope == "all") {
      return true;
    }
    if (scope == "apps") {
      return isRpmAppPackage(name);
    }
    if (scope == "libs") {
      return isRpmLibraryPackage(name);
    }
    if (scope == "devel") {
      return isRpmDevelPackage(name);
    }
    return true;
  }

  void rankRpmSearchHits(std::string_view query, std::vector<PackageHit>& hits) {
    const auto needle = std::string(query);
    if (needle.empty() || hits.size() < 2) {
      sortHits(hits);
      return;
    }
    const auto lower = [](std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (const unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
      }
      return out;
    };
    const auto q = lower(needle);
    const auto score = [&](const PackageHit& hit) {
      int value = 0;
      const auto name = lower(hit.name);
      const auto title = lower(hit.title.empty() ? hit.name : hit.title);
      const auto summary = lower(hit.summary);
      if (name == q) {
        value += 1000;
      } else if (name.starts_with(q)) {
        value += 800;
      } else if (name.find(q) != std::string::npos) {
        value += 600;
      }
      if (title == q) {
        value += 200;
      } else if (title.find(q) != std::string::npos) {
        value += 120;
      }
      if (summary.find(q) != std::string::npos) {
        value += 40;
      }
      if (hit.state == PackageState::Installed) {
        value += 30;
      } else if (hit.state == PackageState::Update) {
        value += 40;
      }
      if (isRpmDevelPackage(hit.name)) {
        value -= 180;
      } else if (isRpmLibraryPackage(hit.name)) {
        value -= 90;
      }
      return value;
    };
    std::ranges::stable_sort(hits, [&](const PackageHit& left, const PackageHit& right) {
      const int leftScore = score(left);
      const int rightScore = score(right);
      if (leftScore != rightScore) {
        return leftScore > rightScore;
      }
      return left.name < right.name;
    });
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

  bool applyFlatpakProgressLine(std::string_view line, PkconProgress& progress) {
    line = trim(line);
    if (line.empty()) {
      return false;
    }
    if (line.starts_with("Installation complete") || line.starts_with("Uninstall complete")
        || line.starts_with("Updates complete")) {
      progress.status = std::string(line);
      progress.percent = 100;
      return true;
    }

    const bool interesting = line.starts_with("Installing") || line.starts_with("Uninstalling")
        || line.starts_with("Updating") || line.starts_with("Downloading") || line.starts_with("Looking")
        || line.starts_with("Fetching") || line.starts_with("Pulling");
    const auto percentAt = line.rfind('%');
    if (!interesting && percentAt == std::string_view::npos) {
      return false;
    }

    if (percentAt != std::string_view::npos && percentAt > 0) {
      std::size_t digitEnd = percentAt;
      while (digitEnd > 0 && std::isdigit(static_cast<unsigned char>(line[digitEnd - 1])) != 0) {
        --digitEnd;
      }
      if (digitEnd < percentAt) {
        int value = 0;
        for (std::size_t i = digitEnd; i < percentAt; ++i) {
          value = value * 10 + (line[i] - '0');
          if (value > 100) {
            value = 100;
            break;
          }
        }
        progress.percent = value;
      }
    }

    std::string_view status = line;
    if (const auto ellipsis = line.find("…"); ellipsis != std::string_view::npos) {
      status = line.substr(0, ellipsis + strlen("…"));
    } else if (const auto dots = line.find("..."); dots != std::string_view::npos) {
      status = line.substr(0, dots + 3);
    } else {
      std::size_t cut = 0;
      while (cut < line.size() && static_cast<unsigned char>(line[cut]) < 0x80) {
        ++cut;
      }
      status = trim(line.substr(0, cut));
    }
    status = trim(status);
    while (!status.empty() && (status.back() == '#' || status.back() == ' ' || status.back() == '\t')) {
      status.remove_suffix(1);
    }
    if (!status.empty()) {
      progress.status = std::string(status);
    } else if (progress.status.empty()) {
      progress.status = "Working…";
    }
    return true;
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
      if (kind == FlatpakListKind::Catalog) {
        hit.info = "Available";
        hit.state = PackageState::Available;
      } else {
        hit.info = "Update";
        hit.state = PackageState::Update;
      }
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
      if (device.contains("Icons") && device["Icons"].is_array()) {
        for (const auto& icon : device["Icons"]) {
          if (icon.is_string() && !icon.get_ref<const std::string&>().empty()) {
            hit.iconName = icon.get<std::string>();
            break;
          }
        }
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

  std::string plainDescription(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    bool pendingBreak = false;
    for (const char ch : text) {
      if (ch == '\n' || ch == '\r') {
        pendingBreak = true;
        pendingSpace = false;
        continue;
      }
      if (ch == ' ' || ch == '\t') {
        pendingSpace = true;
        continue;
      }
      if (pendingBreak && !out.empty()) {
        out.append("\n\n");
      } else if (pendingSpace && !out.empty() && out.back() != '\n') {
        out.push_back(' ');
      }
      pendingSpace = false;
      pendingBreak = false;
      out.push_back(ch);
      if (out.size() >= 8000) {
        break;
      }
    }
    // AppStream descriptions often keep a few named entities after tag stripping.
    auto replaceAll = [](std::string& value, std::string_view from, std::string_view to) {
      std::size_t pos = 0;
      while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
      }
    };
    replaceAll(out, "&apos;", "'");
    replaceAll(out, "&#39;", "'");
    replaceAll(out, "&quot;", "\"");
    replaceAll(out, "&amp;", "&");
    replaceAll(out, "&lt;", "<");
    replaceAll(out, "&gt;", ">");
    replaceAll(out, "&nbsp;", " ");
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
      const auto& leftKey = left.title.empty() ? left.name : left.title;
      const auto& rightKey = right.title.empty() ? right.name : right.title;
      return std::lexicographical_compare(
          leftKey.begin(), leftKey.end(), rightKey.begin(), rightKey.end(),
          [](char leftCh, char rightCh) {
            return std::tolower(static_cast<unsigned char>(leftCh)) < std::tolower(static_cast<unsigned char>(rightCh));
          }
      );
    });
  }

  namespace {

    std::string readFileText(const std::filesystem::path& path) {
      std::ifstream file(path);
      if (!file) {
        return {};
      }
      return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    struct AppstreamFileCache {
      std::filesystem::file_time_type mtime{};
      std::string text;
    };

    struct AppstreamComponentRef {
      std::string xmlPath;
      std::size_t start = 0;
      std::size_t length = 0;
      bool missing = false;
    };

    std::mutex& appstreamCacheMutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::unordered_map<std::string, AppstreamFileCache>& appstreamFileCache() {
      static std::unordered_map<std::string, AppstreamFileCache> cache;
      return cache;
    }

    std::unordered_map<std::string, AppstreamComponentRef>& appstreamComponentIndex() {
      static std::unordered_map<std::string, AppstreamComponentRef> cache;
      return cache;
    }

    const std::string& loadAppstreamXmlCached(const std::string& xmlPath) {
      std::error_code error;
      const auto mtime = std::filesystem::last_write_time(xmlPath, error);
      auto& cached = appstreamFileCache()[xmlPath];
      if (error) {
        cached = {};
        return cached.text;
      }
      if (cached.text.empty() || cached.mtime != mtime) {
        cached.text = readFileText(xmlPath);
        cached.mtime = mtime;
        appstreamComponentIndex().clear();
      }
      return cached.text;
    }

    void rememberAppstreamComponent(std::string appId, AppstreamComponentRef ref) {
      if (!isValidFlatpakAppId(appId)) {
        return;
      }
      appstreamComponentIndex().insert_or_assign(std::move(appId), std::move(ref));
    }

    std::optional<std::string> lookupAppstreamComponent(const std::string& appId) {
      const auto it = appstreamComponentIndex().find(appId);
      if (it == appstreamComponentIndex().end()) {
        return std::nullopt;
      }
      if (it->second.missing || it->second.length == 0) {
        return std::string{};
      }
      const auto file = appstreamFileCache().find(it->second.xmlPath);
      if (file == appstreamFileCache().end()
          || it->second.start + it->second.length > file->second.text.size()) {
        return std::nullopt;
      }
      return file->second.text.substr(it->second.start, it->second.length);
    }

    std::string xmlTextContent(std::string_view xml, std::string_view openTag) {
      const auto open = xml.find(openTag);
      if (open == std::string_view::npos) {
        return {};
      }
      const auto start = open + openTag.size();
      const auto close = xml.find('<', start);
      if (close == std::string_view::npos || close <= start) {
        return {};
      }
      return std::string(trim(xml.substr(start, close - start)));
    }

    std::string stripXmlTags(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      bool inTag = false;
      bool pendingSpace = false;
      for (const char ch : text) {
        if (ch == '<') {
          inTag = true;
          pendingSpace = true;
          continue;
        }
        if (ch == '>') {
          inTag = false;
          continue;
        }
        if (inTag) {
          continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
          pendingSpace = true;
          continue;
        }
        if (pendingSpace && !out.empty()) {
          out.push_back(' ');
        }
        pendingSpace = false;
        out.push_back(ch);
      }
      return out;
    }

    std::string firstTaggedBlock(std::string_view xml, std::string_view open, std::string_view close) {
      const auto start = xml.find(open);
      if (start == std::string_view::npos) {
        return {};
      }
      const auto contentStart = start + open.size();
      const auto end = xml.find(close, contentStart);
      if (end == std::string_view::npos) {
        return {};
      }
      return std::string(xml.substr(contentStart, end - contentStart));
    }

    void appendUnique(std::vector<std::string>& values, std::string value) {
      if (value.empty()) {
        return;
      }
      if (std::ranges::find(values, value) != values.end()) {
        return;
      }
      values.push_back(std::move(value));
    }

    int screenshotWidthHint(std::string_view url) {
      if (url.contains("/1504x") || url.contains("/1248x")) {
        return 3;
      }
      if (url.contains("/752x") || url.contains("/624x")) {
        return 2;
      }
      if (url.contains("/224x") || url.contains("/112x")) {
        return 0;
      }
      return 1;
    }

  } // namespace

  std::vector<std::string> flatpakAppstreamRoots() {
    std::vector<std::string> roots;
    auto addRemote = [&](const std::filesystem::path& remoteRoot) {
      std::error_code error;
      const auto active = remoteRoot / "active";
      if (std::filesystem::is_directory(active, error)) {
        roots.push_back(active.string());
        return;
      }
      if (!std::filesystem::is_directory(remoteRoot, error)) {
        return;
      }
      for (const auto& entry : std::filesystem::directory_iterator(remoteRoot, error)) {
        if (!entry.is_directory(error)) {
          continue;
        }
        roots.push_back(entry.path().string());
      }
    };
    auto scanBase = [&](const std::filesystem::path& base) {
      std::error_code error;
      if (!std::filesystem::is_directory(base, error)) {
        return;
      }
      for (const auto& remote : std::filesystem::directory_iterator(base, error)) {
        if (!remote.is_directory(error)) {
          continue;
        }
        addRemote(remote.path() / "x86_64");
      }
    };
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      scanBase(std::filesystem::path(home) / ".local/share/flatpak/appstream");
    }
    scanBase("/var/lib/flatpak/appstream");
    return roots;
  }

  std::string resolveFlatpakIconPath(std::string_view appId, std::string_view remote) {
    if (!isValidFlatpakAppId(appId)) {
      return {};
    }
    const std::array sizes = {std::string_view{"128x128"}, std::string_view{"64x64"}, std::string_view{"256x256"}};
    const std::array extensions = {std::string_view{".png"}, std::string_view{".svg"}, std::string_view{".jpg"}};
    const auto roots = flatpakAppstreamRoots();
    auto search = [&](bool requireRemote) -> std::string {
      for (const auto& root : roots) {
        if (requireRemote && !remote.empty()) {
          const auto marker = "/" + std::string(remote) + "/";
          if (root.find(marker) == std::string::npos) {
            continue;
          }
        }
        const auto icons = std::filesystem::path(root) / "icons";
        for (const auto size : sizes) {
          for (const auto ext : extensions) {
            std::error_code error;
            const auto path = icons / std::string(size) / (std::string(appId) + std::string(ext));
            if (std::filesystem::is_regular_file(path, error)) {
              return path.string();
            }
          }
        }
      }
      return {};
    };
    if (!remote.empty()) {
      if (auto path = search(true); !path.empty()) {
        return path;
      }
    }
    return search(false);
  }

  void attachFlatpakIcons(std::vector<PackageHit>& hits) {
    if (hits.empty()) {
      return;
    }
    bool missing = false;
    for (const auto& hit : hits) {
      if (hit.iconPath.empty()) {
        missing = true;
        break;
      }
    }
    if (!missing) {
      return;
    }

    // One directory walk per AppStream root instead of per-app filesystem probes.
    std::unordered_map<std::string, std::string> icons;
    icons.reserve(hits.size());
    // Prefer larger icons; emplace keeps the first (largest) path for each app id.
    const std::array sizes = {
        std::string_view{"256x256"}, std::string_view{"128x128"}, std::string_view{"64x64"},
        std::string_view{"48x48"}, std::string_view{"32x32"}
    };
    for (const auto& root : flatpakAppstreamRoots()) {
      for (const auto size : sizes) {
        std::error_code error;
        const auto dir = std::filesystem::path(root) / "icons" / std::string(size);
        if (!std::filesystem::is_directory(dir, error)) {
          continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
          if (!entry.is_regular_file(error)) {
            continue;
          }
          const auto stem = entry.path().stem().string();
          if (isValidFlatpakAppId(stem)) {
            icons.emplace(stem, entry.path().string());
          }
        }
      }
    }
    for (auto& hit : hits) {
      if (!hit.iconPath.empty()) {
        continue;
      }
      if (const auto it = icons.find(hit.name); it != icons.end()) {
        hit.iconPath = it->second;
      }
    }
  }

  std::optional<std::string> extractAppstreamComponentXml(std::string_view xml, std::string_view appId) {
    if (!isValidFlatpakAppId(appId) || xml.empty()) {
      return std::nullopt;
    }
    const auto idTag = "<id>" + std::string(appId) + "</id>";
    const auto idPos = xml.find(idTag);
    if (idPos == std::string_view::npos) {
      return std::nullopt;
    }
    const auto start = xml.rfind("<component", idPos);
    if (start == std::string_view::npos) {
      return std::nullopt;
    }
    const auto end = xml.find("</component>", idPos);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return std::string(xml.substr(start, end + std::string_view("</component>").size() - start));
  }

  PackageDetails parseAppstreamComponentXml(std::string_view componentXml, PackageHit base) {
    PackageDetails details;
    details.hit = std::move(base);
    if (componentXml.empty()) {
      return details;
    }
    if (details.hit.title.empty()) {
      details.hit.title = compactSummary(xmlTextContent(componentXml, "<name>"));
    }
    if (details.hit.summary.empty()) {
      details.hit.summary = compactSummary(xmlTextContent(componentXml, "<summary>"));
    }
    details.license = xmlTextContent(componentXml, "<project_license>");
    details.developer = xmlTextContent(componentXml, "<developer_name>");
    if (details.developer.empty()) {
      const auto developer = firstTaggedBlock(componentXml, "<developer>", "</developer>");
      details.developer = xmlTextContent(developer, "<name>");
      if (details.developer.empty()) {
        details.developer = compactSummary(stripXmlTags(developer));
      }
    }
    const auto description = firstTaggedBlock(componentXml, "<description>", "</description>");
    details.description = plainDescription(stripXmlTags(description));

    std::size_t pos = 0;
    while (pos < componentXml.size()) {
      const auto urlPos = componentXml.find("<url", pos);
      if (urlPos == std::string_view::npos) {
        break;
      }
      const auto typePos = componentXml.find("type=\"", urlPos);
      const auto gt = componentXml.find('>', urlPos);
      if (gt == std::string_view::npos) {
        break;
      }
      std::string type;
      if (typePos != std::string_view::npos && typePos < gt) {
        const auto typeStart = typePos + 6;
        const auto typeEnd = componentXml.find('"', typeStart);
        if (typeEnd != std::string_view::npos && typeEnd < gt) {
          type = std::string(componentXml.substr(typeStart, typeEnd - typeStart));
        }
      }
      const auto close = componentXml.find("</url>", gt);
      if (close == std::string_view::npos) {
        break;
      }
      const auto value = std::string(trim(componentXml.substr(gt + 1, close - gt - 1)));
      if ((value.starts_with("http://") || value.starts_with("https://"))) {
        if (type == "homepage" && details.homepage.empty()) {
          details.homepage = value;
        } else if ((type == "bugtracker" || type == "issue-tracker") && details.bugtracker.empty()) {
          details.bugtracker = value;
        } else if (type == "donation" && details.donation.empty()) {
          details.donation = value;
        } else if ((type == "help" || type == "faq") && details.helpUrl.empty()) {
          details.helpUrl = value;
        }
      }
      pos = close + 6;
    }

    {
      const auto ratingPos = componentXml.find("<content_rating");
      if (ratingPos != std::string_view::npos) {
        const auto ratingEnd = componentXml.find("</content_rating>", ratingPos);
        if (ratingEnd != std::string_view::npos) {
          const auto ratingBlock = componentXml.substr(ratingPos, ratingEnd - ratingPos);
          std::string rating;
          const auto typePos = ratingBlock.find("type=\"");
          if (typePos != std::string_view::npos) {
            const auto typeStart = typePos + 6;
            const auto typeEnd = ratingBlock.find('"', typeStart);
            if (typeEnd != std::string_view::npos) {
              rating = std::string(ratingBlock.substr(typeStart, typeEnd - typeStart));
            }
          }
          unsigned intense = 0;
          unsigned mild = 0;
          std::size_t attrPos = 0;
          while (attrPos < ratingBlock.size()) {
            const auto open = ratingBlock.find("<content_attribute", attrPos);
            if (open == std::string_view::npos) {
              break;
            }
            const auto gt = ratingBlock.find('>', open);
            const auto close = ratingBlock.find("</content_attribute>", open);
            if (gt == std::string_view::npos || close == std::string_view::npos || gt > close) {
              break;
            }
            auto value = trim(ratingBlock.substr(gt + 1, close - gt - 1));
            if (const auto valueAttr = ratingBlock.substr(open, gt - open); valueAttr.contains("value=\"")) {
              const auto valueStart = valueAttr.find("value=\"") + 7;
              const auto valueEnd = valueAttr.find('"', valueStart);
              if (valueEnd != std::string_view::npos) {
                value = trim(valueAttr.substr(valueStart, valueEnd - valueStart));
              }
            }
            if (value == "intense") {
              ++intense;
            } else if (value == "mild" || value == "moderate") {
              ++mild;
            }
            attrPos = close + std::string_view("</content_attribute>").size();
          }
          if (rating.starts_with("oars-")) {
            if (intense > 0) {
              details.contentRating = "May include intense content";
            } else if (mild > 0) {
              details.contentRating = "May include mild content";
            } else {
              details.contentRating = "No objectionable content listed";
            }
          } else if (!rating.empty()) {
            details.contentRating = rating;
          }
        }
      }
    }

    pos = 0;
    while (pos < componentXml.size()) {
      const auto catPos = componentXml.find("<category>", pos);
      if (catPos == std::string_view::npos) {
        break;
      }
      const auto start = catPos + std::string_view("<category>").size();
      const auto end = componentXml.find("</category>", start);
      if (end == std::string_view::npos) {
        break;
      }
      const auto category = std::string(trim(componentXml.substr(start, end - start)));
      appendUnique(details.categories, category);
      appendUnique(details.hit.categories, category);
      pos = end + std::string_view("</category>").size();
    }

    std::vector<std::pair<int, std::string>> rankedScreens;
    pos = 0;
    while (pos < componentXml.size()) {
      const auto imagePos = componentXml.find("<image", pos);
      if (imagePos == std::string_view::npos) {
        break;
      }
      const auto gt = componentXml.find('>', imagePos);
      if (gt == std::string_view::npos) {
        break;
      }
      const auto close = componentXml.find("</image>", gt);
      if (close == std::string_view::npos) {
        break;
      }
      auto url = std::string(trim(componentXml.substr(gt + 1, close - gt - 1)));
      if (url.starts_with("http://") || url.starts_with("https://")) {
        rankedScreens.emplace_back(screenshotWidthHint(url), std::move(url));
      }
      pos = close + std::string_view("</image>").size();
    }
    std::ranges::stable_sort(rankedScreens, [](const auto& left, const auto& right) {
      return left.first > right.first;
    });
    for (auto& [rank, url] : rankedScreens) {
      (void)rank;
      appendUnique(details.screenshotUrls, std::move(url));
      if (details.screenshotUrls.size() >= 6) {
        break;
      }
    }

    {
      const auto custom = firstTaggedBlock(componentXml, "<custom>", "</custom>");
      if (custom.contains("flathub::verification::verified\">true")
          || custom.contains("flathub::verification::verified\">True")) {
        std::string method;
        std::string website;
        const auto methodKey = custom.find("flathub::verification::method\">");
        if (methodKey != std::string::npos) {
          const auto start = methodKey + std::string_view("flathub::verification::method\">").size();
          const auto end = custom.find('<', start);
          if (end != std::string::npos) {
            method = std::string(trim(std::string_view(custom).substr(start, end - start)));
          }
        }
        const auto websiteKey = custom.find("flathub::verification::website\">");
        if (websiteKey != std::string::npos) {
          const auto start = websiteKey + std::string_view("flathub::verification::website\">").size();
          const auto end = custom.find('<', start);
          if (end != std::string::npos) {
            website = std::string(trim(std::string_view(custom).substr(start, end - start)));
          }
        }
        details.hit.verified = true;
        if (!website.empty()) {
          details.verification = "Verified publisher · " + website;
        } else if (method == "login_provider" || method == "website") {
          details.verification = "Verified on Flathub";
        } else {
          details.verification = "Verified on Flathub";
        }
      }
    }

    {
      const auto releases = firstTaggedBlock(componentXml, "<releases>", "</releases>");
      if (!releases.empty()) {
        const auto release = firstTaggedBlock(releases, "<release", "</release>");
        if (!release.empty()) {
          std::string version;
          const auto versionPos = release.find("version=\"");
          if (versionPos != std::string_view::npos) {
            const auto versionStart = versionPos + 9;
            const auto versionEnd = release.find('"', versionStart);
            if (versionEnd != std::string_view::npos) {
              version = std::string(release.substr(versionStart, versionEnd - versionStart));
            }
          }
          const auto releaseDescription = firstTaggedBlock(release, "<description>", "</description>");
          auto notes = plainDescription(stripXmlTags(releaseDescription));
          if (!version.empty() && !notes.empty()) {
            details.releaseNotes = version + "\n" + notes;
          } else if (!notes.empty()) {
            details.releaseNotes = std::move(notes);
          } else if (!version.empty()) {
            details.releaseNotes = "Version " + version;
          }
          if (details.releaseNotes.size() > 2000) {
            details.releaseNotes.resize(2000);
          }
        }
      }
    }

    if (details.hit.iconPath.empty() && isValidFlatpakAppId(details.hit.name)) {
      details.hit.iconPath = resolveFlatpakIconPath(details.hit.name, preferredFlatpakRemote(details.hit.repo));
      if (details.hit.iconPath.empty()) {
        details.hit.iconPath = resolveFlatpakIconPath(details.hit.name);
      }
    }
    return details;
  }

  PackageDetails parseFlatpakRemoteInfo(std::string_view text, PackageHit base) {
    PackageDetails details;
    details.hit = std::move(base);
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      line = trim(line);
      const auto colon = line.find(':');
      if (colon != std::string_view::npos) {
        auto key = std::string(trim(line.substr(0, colon)));
        auto value = std::string(trim(line.substr(colon + 1)));
        for (char& ch : key) {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (key == "id" && details.hit.name.empty()) {
          details.hit.name = value;
        } else if (key == "version" && details.hit.version.empty()) {
          details.hit.version = value;
        } else if (key == "license" && details.license.empty()) {
          details.license = value;
        } else if (key == "download size") {
          details.downloadSize = value;
        } else if (key == "installed size") {
          details.installedSize = value;
        } else if (key == "runtime") {
          details.runtime = value;
        } else if (key == "branch" && details.hit.release.empty()) {
          details.hit.release = value;
        }
      } else if (!line.empty() && details.hit.title.empty() && details.hit.summary.empty()) {
        details.hit.title = compactSummary(line);
        details.hit.summary = details.hit.title;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return details;
  }

  PackageDetails parseFlatpakPermissions(std::string_view text, PackageDetails details) {
    auto labels = humanizeFlatpakPermissions(text);
    if (labels.empty()) {
      details.permissions = plainDescription(text);
    } else {
      details.permissions.clear();
      for (const auto& label : labels) {
        if (!details.permissions.empty()) {
          details.permissions.push_back('\n');
        }
        details.permissions += label;
      }
    }
    if (details.permissions.size() > 4000) {
      details.permissions.resize(4000);
    }
    return details;
  }

  std::vector<std::string> humanizeFlatpakPermissions(std::string_view metadata) {
    std::vector<std::string> labels;
    auto add = [&](std::string_view label) { appendUnique(labels, std::string(label)); };
    auto parseList = [&](std::string_view key, auto mapValue) {
      const auto needle = std::string(key) + "=";
      std::size_t pos = 0;
      while (pos < metadata.size()) {
        const auto found = metadata.find(needle, pos);
        if (found == std::string_view::npos) {
          return;
        }
        if (found > 0) {
          const char before = metadata[found - 1];
          if (before != '\n' && before != '\r' && before != '\t' && before != ' ') {
            pos = found + needle.size();
            continue;
          }
        }
        const auto start = found + needle.size();
        const auto end = metadata.find('\n', start);
        auto values = metadata.substr(start, end == std::string_view::npos ? metadata.size() - start : end - start);
        std::size_t item = 0;
        while (item <= values.size()) {
          const auto sep = values.find(';', item);
          auto token = trim(values.substr(item, sep == std::string_view::npos ? values.size() - item : sep - item));
          if (!token.empty()) {
            mapValue(token);
          }
          if (sep == std::string_view::npos) {
            break;
          }
          item = sep + 1;
        }
        return;
      }
    };

    parseList("shared", [&](std::string_view token) {
      if (token == "network") {
        add("Has network access");
      } else if (token == "ipc") {
        add("Can communicate with other apps on this device");
      }
    });
    parseList("sockets", [&](std::string_view token) {
      if (token == "x11") {
        add("Can access the legacy X11 display");
      } else if (token == "fallback-x11") {
        add("Can use X11 when Wayland is unavailable");
      } else if (token == "wayland") {
        add("Can access the Wayland display");
      } else if (token == "pulseaudio") {
        add("Can play and record audio");
      } else if (token == "session-bus") {
        add("Can talk to the session bus");
      } else if (token == "system-bus") {
        add("Can talk to the system bus");
      } else if (token == "ssh-auth") {
        add("Can use SSH agent authentication");
      } else if (token == "pcsc") {
        add("Can access smart cards");
      } else if (token == "cups") {
        add("Can access printers");
      } else if (token == "gpg-agent") {
        add("Can use the GPG agent");
      }
    });
    parseList("devices", [&](std::string_view token) {
      if (token == "all") {
        add("Can access all devices");
      } else if (token == "dri") {
        add("Can use GPU acceleration");
      } else if (token == "kvm") {
        add("Can use KVM virtualization");
      } else if (token == "shm") {
        add("Can use shared memory devices");
      } else if (token == "input") {
        add("Can access input devices");
      }
    });
    parseList("features", [&](std::string_view token) {
      if (token == "bluetooth") {
        add("Can use Bluetooth");
      } else if (token == "devel") {
        add("Development mode is enabled");
      } else if (token == "multiarch") {
        add("Can run multiarch binaries");
      } else if (token == "canbus") {
        add("Can access CAN bus");
      }
    });
    parseList("filesystems", [&](std::string_view token) {
      if (token == "home" || token == "home:rw") {
        add("Can read and write your home folder");
      } else if (token == "home:ro") {
        add("Can read your home folder");
      } else if (token == "host" || token == "host:rw") {
        add("Can read and write all system files");
      } else if (token == "host:ro") {
        add("Can read all system files");
      } else if (token == "xdg-download" || token == "xdg-download:rw") {
        add("Can access your Downloads folder");
      } else if (token == "xdg-documents" || token == "xdg-documents:rw") {
        add("Can access your Documents folder");
      } else if (token == "xdg-pictures" || token == "xdg-pictures:rw") {
        add("Can access your Pictures folder");
      } else if (token == "xdg-music" || token == "xdg-music:rw") {
        add("Can access your Music folder");
      } else if (token == "xdg-videos" || token == "xdg-videos:rw") {
        add("Can access your Videos folder");
      } else if (token == "xdg-desktop" || token == "xdg-desktop:rw") {
        add("Can access your Desktop folder");
      } else if (token == "xdg-run/dconf" || token.starts_with("xdg-config")) {
        add("Can access configuration files");
      } else if (token.starts_with("~/") || token.starts_with("/home")) {
        add("Can access selected home folders");
      } else if (!token.empty()) {
        add("Can access selected files or folders");
      }
    });
    if (metadata.contains("Session Bus Policy") || metadata.contains("[Session Bus Policy]")) {
      add("Can communicate with other session services");
    }
    if (metadata.contains("System Bus Policy") || metadata.contains("[System Bus Policy]")) {
      add("Can communicate with system services");
    }
    return labels;
  }

  std::string categoryDisplayName(std::string_view category) {
    if (category == "AudioVideo") {
      return "Media";
    }
    if (category == "Audio") {
      return "Audio";
    }
    if (category == "Video") {
      return "Video";
    }
    if (category == "Development") {
      return "Development";
    }
    if (category == "Education") {
      return "Education";
    }
    if (category == "Game") {
      return "Games";
    }
    if (category == "Graphics") {
      return "Graphics";
    }
    if (category == "Network") {
      return "Network";
    }
    if (category == "Office") {
      return "Office";
    }
    if (category == "Science") {
      return "Science";
    }
    if (category == "System") {
      return "System";
    }
    if (category == "Utility") {
      return "Utilities";
    }
    if (category == "Settings") {
      return "Settings";
    }
    if (category == "Security") {
      return "Security";
    }
    return std::string(category);
  }

  FlatpakCatalogIndex loadFlatpakCatalogIndex() {
    FlatpakCatalogIndex index;
    auto roots = flatpakAppstreamRoots();
    // Prefer Flathub roots so a small fedora/user index does not hide the main catalog.
    std::ranges::stable_partition(roots, [](const std::string& root) {
      return root.find("/flathub/") != std::string::npos;
    });
    std::scoped_lock lock(appstreamCacheMutex());
    for (const auto& root : roots) {
      const auto xmlPath = (std::filesystem::path(root) / "appstream.xml").string();
      const auto& text = loadAppstreamXmlCached(xmlPath);
      if (text.empty()) {
        continue;
      }
      std::size_t pos = 0;
      while (pos < text.size()) {
        const auto idOpen = text.find("<id>", pos);
        if (idOpen == std::string::npos) {
          break;
        }
        const auto idStart = idOpen + 4;
        const auto idClose = text.find("</id>", idStart);
        if (idClose == std::string::npos) {
          break;
        }
        const auto appId = std::string(trim(std::string_view(text).substr(idStart, idClose - idStart)));
        const auto componentStart = text.rfind("<component", idOpen);
        const auto componentEnd = text.find("</component>", idClose);
        if (componentStart == std::string::npos || componentEnd == std::string::npos) {
          break;
        }
        // Always advance past this component so nested/invalid <id> tags cannot stall the scan.
        pos = componentEnd + std::string_view("</component>").size();
        if (!isValidFlatpakAppId(appId)) {
          continue;
        }
        const auto componentLength = componentEnd + std::string_view("</component>").size() - componentStart;
        if (!appstreamComponentIndex().contains(appId)) {
          rememberAppstreamComponent(
              appId, AppstreamComponentRef{.xmlPath = xmlPath, .start = componentStart, .length = componentLength}
          );
        }
        const auto block = std::string_view(text).substr(idClose, componentEnd - idClose);
        if (block.contains("flathub::verification::verified\">true")
            || block.contains("flathub::verification::verified\">True")) {
          index.verified.insert(appId);
        }
        if (!index.categories.contains(appId)) {
          std::vector<std::string> categories;
          std::size_t catPos = 0;
          while (catPos < block.size()) {
            const auto open = block.find("<category>", catPos);
            if (open == std::string_view::npos) {
              break;
            }
            const auto start = open + std::string_view("<category>").size();
            const auto close = block.find("</category>", start);
            if (close == std::string_view::npos) {
              break;
            }
            appendUnique(categories, std::string(trim(block.substr(start, close - start))));
            catPos = close + std::string_view("</category>").size();
          }
          if (!categories.empty()) {
            index.categories.emplace(appId, std::move(categories));
          }
        }
      }
    }
    return index;
  }

  std::unordered_map<std::string, std::vector<std::string>> loadFlatpakCategoryIndex() {
    return loadFlatpakCatalogIndex().categories;
  }

  void attachFlatpakCategories(
      std::vector<PackageHit>& hits, const std::unordered_map<std::string, std::vector<std::string>>& index
  ) {
    for (auto& hit : hits) {
      if (!hit.categories.empty()) {
        continue;
      }
      const auto it = index.find(hit.name);
      if (it != index.end()) {
        hit.categories = it->second;
      }
    }
  }

  void attachFlatpakCatalogIndex(std::vector<PackageHit>& hits, const FlatpakCatalogIndex& index) {
    attachFlatpakCategories(hits, index.categories);
    for (auto& hit : hits) {
      if (!hit.verified && index.verified.contains(hit.name)) {
        hit.verified = true;
      }
    }
  }

  PackageDetails parseRpmInfoOutput(std::string_view text, PackageHit base) {
    PackageDetails details;
    details.hit = std::move(base);
    std::string* capture = nullptr;
    std::string description;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
      if (capture == &description) {
        if (!line.empty() && line.find(':') != std::string_view::npos) {
          const auto key = trim(line.substr(0, line.find(':')));
          const bool looksLikeField = key == "Name" || key == "Version" || key == "Release" || key == "Architecture"
              || key == "Install Date" || key == "Group" || key == "Size" || key == "License" || key == "Signature"
              || key == "Source RPM" || key == "Build Date" || key == "Build Host" || key == "Relocations"
              || key == "Packager" || key == "Vendor" || key == "URL" || key == "Summary" || key == "Description";
          if (looksLikeField) {
            capture = nullptr;
          }
        }
      }
      if (capture == &description) {
        if (!description.empty()) {
          description.push_back('\n');
        }
        description.append(line);
      } else {
        line = trim(line);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
          const auto key = trim(line.substr(0, colon));
          const auto value = std::string(trim(line.substr(colon + 1)));
          if (key == "Name" && details.hit.name.empty()) {
            details.hit.name = value;
          } else if (key == "Summary") {
            details.hit.summary = compactSummary(value);
            if (details.hit.title.empty()) {
              details.hit.title = details.hit.summary;
            }
          } else if (key == "Version" && details.hit.version.empty()) {
            details.hit.version = value;
          } else if (key == "Release" && details.hit.release.empty()) {
            details.hit.release = value;
          } else if ((key == "Architecture" || key == "Arch") && details.hit.arch.empty()) {
            details.hit.arch = value;
          } else if (key == "License") {
            details.license = value;
          } else if (key == "URL") {
            details.homepage = value;
          } else if (key == "Size") {
            details.installedSize = value;
          } else if (key == "Vendor" || key == "Packager") {
            if (details.developer.empty()) {
              details.developer = value;
            }
          } else if (key == "Description") {
            capture = &description;
            if (!value.empty()) {
              description = value;
            }
          }
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    details.description = plainDescription(description);
    return details;
  }

  PackageDetails enrichFromFwupdDeviceJson(std::string_view text, PackageHit base) {
    PackageDetails details;
    details.hit = std::move(base);
    auto jsonText = std::string(text);
    if (const auto brace = jsonText.find('{'); brace != std::string::npos && brace > 0) {
      jsonText.erase(0, brace);
    }
    const auto json = nlohmann::json::parse(jsonText, nullptr, false);
    if (!json.is_object() || !json.contains("Devices") || !json["Devices"].is_array()) {
      return details;
    }
    for (const auto& device : json["Devices"]) {
      if (!device.is_object()) {
        continue;
      }
      if (jsonString(device, "DeviceId") != details.hit.name) {
        continue;
      }
      if (details.hit.title.empty()) {
        details.hit.title = compactSummary(jsonString(device, "Name"));
      }
      if (details.hit.summary.empty()) {
        details.hit.summary = compactSummary(jsonString(device, "Summary"));
      }
      details.developer = jsonString(device, "Vendor");
      if (details.hit.version.empty()) {
        details.hit.version = jsonString(device, "Version");
      }
      if (device.contains("Icons") && device["Icons"].is_array()) {
        for (const auto& icon : device["Icons"]) {
          if (icon.is_string() && !icon.get_ref<const std::string&>().empty()) {
            details.hit.iconName = icon.get<std::string>();
            break;
          }
        }
      }
      if (device.contains("Releases") && device["Releases"].is_array() && !device["Releases"].empty()) {
        const auto& release = device["Releases"].front();
        if (release.is_object()) {
          auto notes = jsonString(release, "Description");
          if (notes.empty()) {
            notes = jsonString(release, "Summary");
          }
          details.description = plainDescription(stripHtml(notes));
          if (const auto version = jsonString(release, "Version"); !version.empty()) {
            details.hit.version = version;
          }
        }
      }
      if (details.description.empty()) {
        details.description = plainDescription(details.hit.summary);
      }
      break;
    }
    return details;
  }

  PackageDetails loadFlatpakAppstreamDetails(PackageHit base) {
    if (!isValidFlatpakAppId(base.name)) {
      PackageDetails details;
      details.hit = std::move(base);
      return details;
    }

    std::optional<std::string> component;
    {
      std::scoped_lock lock(appstreamCacheMutex());
      component = lookupAppstreamComponent(base.name);
      if (!component) {
        for (const auto& root : flatpakAppstreamRoots()) {
          const auto xmlPath = (std::filesystem::path(root) / "appstream.xml").string();
          const auto& text = loadAppstreamXmlCached(xmlPath);
          if (text.empty()) {
            continue;
          }
          if (auto extracted = extractAppstreamComponentXml(text, base.name)) {
            const auto start = text.find(*extracted);
            if (start != std::string::npos) {
              rememberAppstreamComponent(
                  base.name,
                  AppstreamComponentRef{.xmlPath = xmlPath, .start = start, .length = extracted->size()}
              );
            }
            component = std::move(extracted);
            break;
          }
        }
        if (!component) {
          rememberAppstreamComponent(base.name, AppstreamComponentRef{.missing = true});
          component = std::string{};
        }
      }
    }

    if (component && !component->empty()) {
      return parseAppstreamComponentXml(*component, std::move(base));
    }
    PackageDetails details;
    details.hit = std::move(base);
    if (details.hit.iconPath.empty()) {
      details.hit.iconPath = resolveFlatpakIconPath(details.hit.name, preferredFlatpakRemote(details.hit.repo));
      if (details.hit.iconPath.empty()) {
        details.hit.iconPath = resolveFlatpakIconPath(details.hit.name);
      }
    }
    return details;
  }

} // namespace helix::packages

