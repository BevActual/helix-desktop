#include "system/fedora_upgrade.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

  std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
      return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  std::string osReleaseText() {
    for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}) {
      auto text = readTextFile(path);
      if (!text.empty()) {
        return text;
      }
    }
    return {};
  }

  bool rpmQuietInstalled(std::string_view name) {
    const pid_t pid = ::fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      const std::string package(name);
      ::execl("/usr/bin/rpm", "rpm", "-q", "--quiet", package.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
      return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  std::vector<std::string> installedAllowlistedPackages() {
    std::vector<std::string> installed;
    for (const auto name : helix::fedora_upgrade::kAllowlistedPackages) {
      if (rpmQuietInstalled(name)) {
        installed.emplace_back(name);
      }
    }
    return installed;
  }

  void printHelp() {
    std::cerr << "helix-system-upgrade\n"
              << "Upgrade the hardcoded Helix Shell Fedora packages via dnf.\n"
              << "Run through pkexec. Extra arguments are rejected.\n";
  }

  const char* statusMessage(helix::fedora_upgrade::HelperStatus status) {
    using helix::fedora_upgrade::HelperStatus;
    switch (status) {
    case HelperStatus::ExtraArguments:
      return "this helper does not accept arguments";
    case HelperStatus::NotRoot:
      return "must run as root via pkexec";
    case HelperStatus::NotFedora:
      return "only Fedora is supported";
    case HelperStatus::Atomic:
      return "Fedora Atomic images are not supported; use rpm-ostree";
    case HelperStatus::NoPackages:
      return "no Helix Shell packages are installed";
    case HelperStatus::Help:
    case HelperStatus::Ready:
      return "";
    }
    return "upgrade helper failed";
  }

  int execDnf(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(args.front().c_str(), argv.data());
    std::cerr << "execv " << args.front() << ": " << std::strerror(errno) << '\n';
    return 1;
  }

} // namespace

int main(int argc, char* argv[]) {
  using namespace helix::fedora_upgrade;

  const auto plan = planHelper(
      argc, argv, ::geteuid() == 0, osReleaseText(), std::filesystem::exists("/run/ostree-booted"),
      installedAllowlistedPackages()
  );
  if (plan.status == HelperStatus::Help) {
    printHelp();
    return 0;
  }
  if (plan.status != HelperStatus::Ready) {
    std::cerr << statusMessage(plan.status) << '\n';
    return 1;
  }
  return execDnf(plan.dnfArgv);
}
