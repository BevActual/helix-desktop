#include "system/fedora_upgrade.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace noctalia::fedora_upgrade {

  namespace {

    constexpr std::array kAtomicVariants = {
        std::string_view{"silverblue"}, std::string_view{"kinoite"}, std::string_view{"sericea"},
        std::string_view{"onyx"},       std::string_view{"cosmic"},  std::string_view{"coreos"},
        std::string_view{"iot"},
    };

    std::string_view trim(std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
      }
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
      }
      return text;
    }

    std::string_view unquote(std::string_view value) {
      if (value.size() >= 2
          && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        value.remove_prefix(1);
        value.remove_suffix(1);
      }
      return value;
    }

    bool isHelpRequest(int argc, char* const argv[]) {
      return argc == 2 && argv != nullptr && argv[1] != nullptr && std::strcmp(argv[1], "--help") == 0;
    }

  } // namespace

  std::string osReleaseValue(std::string_view text, std::string_view key) {
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
      const auto lineEnd = text.find('\n', lineStart);
      const auto line = trim(text.substr(lineStart, lineEnd == std::string_view::npos ? text.size() - lineStart : lineEnd - lineStart));
      if (line.size() > key.size() && line[key.size()] == '=' && line.substr(0, key.size()) == key) {
        return std::string(unquote(line.substr(key.size() + 1)));
      }
      if (lineEnd == std::string_view::npos) {
        break;
      }
      lineStart = lineEnd + 1;
    }
    return {};
  }

  HostKind classifyHost(std::string_view osRelease, bool ostreeBooted) {
    if (ostreeBooted) {
      return HostKind::FedoraAtomic;
    }
    if (osReleaseValue(osRelease, "ID") != "fedora") {
      return HostKind::Other;
    }
    const auto variant = osReleaseValue(osRelease, "VARIANT_ID");
    if (std::ranges::find(kAtomicVariants, std::string_view{variant}) != kAtomicVariants.end()) {
      return HostKind::FedoraAtomic;
    }
    return HostKind::Fedora;
  }

  std::vector<std::string> selectInstalledPackages(std::span<const std::string> installedNames) {
    std::vector<std::string> selected;
    for (const auto allowlisted : kAllowlistedPackages) {
      if (std::ranges::find(installedNames, allowlisted) != installedNames.end()) {
        selected.emplace_back(allowlisted);
      }
    }
    return selected;
  }

  std::vector<std::string> dnfUpgradeArgv(std::span<const std::string> packages) {
    std::vector<std::string> argv{std::string(kDnfPath), "upgrade", "-y", "--refresh"};
    argv.insert(argv.end(), packages.begin(), packages.end());
    return argv;
  }

  HelperPlan planHelper(
      int argc, char* const argv[], bool isRoot, std::string_view osRelease, bool ostreeBooted,
      std::span<const std::string> installedPackages
  ) {
    HelperPlan plan;
    if (argc < 1 || (argc > 1 && !isHelpRequest(argc, argv))) {
      plan.status = HelperStatus::ExtraArguments;
      return plan;
    }
    if (isHelpRequest(argc, argv)) {
      plan.status = HelperStatus::Help;
      return plan;
    }
    if (!isRoot) {
      plan.status = HelperStatus::NotRoot;
      return plan;
    }
    const auto host = classifyHost(osRelease, ostreeBooted);
    if (host == HostKind::Other) {
      plan.status = HelperStatus::NotFedora;
      return plan;
    }
    if (host == HostKind::FedoraAtomic) {
      plan.status = HelperStatus::Atomic;
      return plan;
    }
    const auto packages = selectInstalledPackages(installedPackages);
    if (packages.empty()) {
      plan.status = HelperStatus::NoPackages;
      return plan;
    }
    plan.status = HelperStatus::Ready;
    plan.dnfArgv = dnfUpgradeArgv(packages);
    return plan;
  }

} // namespace noctalia::fedora_upgrade
