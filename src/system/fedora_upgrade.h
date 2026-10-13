#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace helix::fedora_upgrade {

  inline constexpr std::string_view kHelperName = "helix-system-upgrade";
  inline constexpr std::string_view kDnfPath = "/usr/bin/dnf";
  inline constexpr std::string_view kPkexecName = "pkexec";

  // Hardcoded RPM names the privileged helper may pass to dnf. Order is the
  // upgrade order. Names not installed on the machine are skipped.
  inline constexpr std::string_view kAllowlistedPackages[] = {"helix-shell", "helix", "helix-greeter"};

  enum class HostKind : std::uint8_t { Fedora, FedoraAtomic, Other };

  enum class HelperStatus : std::uint8_t {
    Ready,
    Help,
    ExtraArguments,
    NotRoot,
    NotFedora,
    Atomic,
    NoPackages,
  };

  struct HelperPlan {
    HelperStatus status = HelperStatus::NotFedora;
    std::vector<std::string> dnfArgv;
  };

  [[nodiscard]] std::string osReleaseValue(std::string_view text, std::string_view key);
  [[nodiscard]] HostKind classifyHost(std::string_view osRelease, bool ostreeBooted);
  [[nodiscard]] std::vector<std::string> selectInstalledPackages(std::span<const std::string> installedNames);
  [[nodiscard]] std::vector<std::string> dnfUpgradeArgv(std::span<const std::string> packages);
  [[nodiscard]] HelperPlan planHelper(
      int argc, char* const argv[], bool isRoot, std::string_view osRelease, bool ostreeBooted,
      std::span<const std::string> installedPackages
  );

} // namespace helix::fedora_upgrade
