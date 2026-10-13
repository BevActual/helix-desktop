#include "system/fedora_upgrade.h"

#include <print>
#include <string>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "fedora_upgrade_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  char* kHelpArgv[] = {const_cast<char*>("helix-system-upgrade"), const_cast<char*>("--help"), nullptr};
  char* kBadArgv[] = {const_cast<char*>("helix-system-upgrade"), const_cast<char*>("htop"), nullptr};
  char* kPlainArgv[] = {const_cast<char*>("helix-system-upgrade"), nullptr};

} // namespace

int main() {
  using namespace helix::fedora_upgrade;
  bool ok = true;

  const std::string fedora = "NAME=\"Fedora Linux\"\nID=fedora\nVARIANT_ID=workstation\n";
  const std::string silverblue = "ID=fedora\nVARIANT_ID=silverblue\n";
  const std::string arch = "ID=arch\n";
  const std::string quoted = "ID=\"fedora\"\n";

  ok = expect(osReleaseValue(fedora, "ID") == "fedora", "plain ID") && ok;
  ok = expect(osReleaseValue(quoted, "ID") == "fedora", "quoted ID") && ok;
  ok = expect(classifyHost(fedora, false) == HostKind::Fedora, "workstation") && ok;
  ok = expect(classifyHost(fedora, true) == HostKind::FedoraAtomic, "ostree booted") && ok;
  ok = expect(classifyHost(silverblue, false) == HostKind::FedoraAtomic, "silverblue") && ok;
  ok = expect(classifyHost(arch, false) == HostKind::Other, "arch") && ok;

  const std::vector<std::string> installed{"htop", "helix", "firefox"};
  ok = expect(selectInstalledPackages(installed) == std::vector<std::string>{"helix"}, "allowlist filter") && ok;
  ok = expect(selectInstalledPackages({}).empty(), "empty installed") && ok;

  const auto all = std::vector<std::string>{"helix-shell", "helix", "helix-greeter"};
  ok = expect(selectInstalledPackages(all) == all, "preserve allowlist order") && ok;

  const auto argv = dnfUpgradeArgv(std::vector<std::string>{"helix"});
  ok = expect(argv.size() == 5 && argv[0] == "/usr/bin/dnf" && argv[1] == "upgrade" && argv[4] == "helix", "dnf argv")
      && ok;

  ok = expect(planHelper(2, kHelpArgv, false, fedora, false, installed).status == HelperStatus::Help, "help") && ok;
  ok = expect(
           planHelper(2, kBadArgv, true, fedora, false, installed).status == HelperStatus::ExtraArguments,
           "rejects extra args"
       )
      && ok;
  ok = expect(planHelper(1, kPlainArgv, false, fedora, false, installed).status == HelperStatus::NotRoot, "not root")
      && ok;
  ok = expect(planHelper(1, kPlainArgv, true, arch, false, installed).status == HelperStatus::NotFedora, "not fedora")
      && ok;
  ok = expect(planHelper(1, kPlainArgv, true, silverblue, false, installed).status == HelperStatus::Atomic, "atomic")
      && ok;
  ok = expect(
           planHelper(1, kPlainArgv, true, fedora, false, std::vector<std::string>{"htop"}).status
               == HelperStatus::NoPackages,
           "no packages"
       )
      && ok;

  const auto ready = planHelper(1, kPlainArgv, true, fedora, false, installed);
  ok = expect(ready.status == HelperStatus::Ready, "ready") && ok;
  ok = expect(!ready.dnfArgv.empty() && ready.dnfArgv.back() == "helix", "ready argv") && ok;

  return ok ? 0 : 1;
}
