#include "system/package_spec.h"

#include <print>
#include <string_view>
#include <vector>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "package_spec_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

} // namespace

int main() {
  using namespace noctalia::packages;
  bool ok = true;

  ok = expect(isValidRpmName("helix"), "plain name") && ok;
  ok = expect(isValidRpmName("libstdc++"), "plus plus") && ok;
  ok = expect(isValidRpmName("pkg-name.1_2"), "dotted") && ok;
  ok = expect(!isValidRpmName(""), "empty name") && ok;
  ok = expect(!isValidRpmName("-leading"), "leading dash") && ok;
  ok = expect(!isValidRpmName("bad;name"), "metachar") && ok;

  ok = expect(isSafeSearchQuery("ht"), "min length") && ok;
  ok = expect(isSafeSearchQuery("helix shell"), "space") && ok;
  ok = expect(!isSafeSearchQuery("x"), "too short") && ok;
  ok = expect(!isSafeSearchQuery("foo; rm"), "semicolon") && ok;
  ok = expect(!isSafeSearchQuery("*"), "glob") && ok;
  ok = expect(hasSafeSearchChars("h"), "one char") && ok;
  ok = expect(!hasSafeSearchChars("h;"), "bad char") && ok;

  ok = expect(isProtectedPackage("noctalia"), "noctalia") && ok;
  ok = expect(isProtectedPackage("noctalia-greeter"), "greeter") && ok;
  ok = expect(isProtectedPackage("helix-shell"), "helix-shell") && ok;
  ok = expect(isProtectedPackage("filesystem"), "essential filesystem protected") && ok;
  ok = expect(isProtectedPackage("systemd"), "session manager protected") && ok;
  ok = expect(isProtectedPackage("helix-niri"), "active compositor protected") && ok;
  ok = expect(!isProtectedPackage("systemd-devel"), "development package removable") && ok;

  ok = expect(!isProtectedPackage("helix"), "editor") && ok;

  const auto installed = parsePkconLine("Installed    helix-25.07.1-11.fc44.x86_64 (fedora)");
  ok = expect(installed.has_value(), "installed line") && ok;
  ok = expect(installed && installed->name == "helix" && installed->version == "25.07.1", "installed nevra") && ok;
  ok = expect(installed && installed->release == "11.fc44" && installed->arch == "x86_64", "installed arch") && ok;
  ok = expect(installed && installed->repo == "fedora" && installed->state == PackageState::Installed, "installed state")
      && ok;

  const auto epoch = parsePkconLine("Available     vim-data-2:9.2.1061-1.fc44.noarch (updates)");
  ok = expect(epoch && epoch->name == "vim-data" && epoch->epoch == "2", "epoch name") && ok;
  ok = expect(epoch && epoch->version == "9.2.1061" && epoch->release == "1.fc44", "epoch version") && ok;
  ok = expect(epoch && epoch->state == PackageState::Available, "available state") && ok;

  const auto bugfix = parsePkconLine("Bug fix      foo-1.0-1.fc44.x86_64 (updates)");
  ok = expect(bugfix && bugfix->name == "foo" && bugfix->state == PackageState::Update, "bug fix") && ok;

  const auto enhancement = parsePkconLine("Enhancement  bar-2-1.fc44.x86_64 (updates)");
  ok = expect(enhancement && enhancement->name == "bar" && enhancement->state == PackageState::Update, "enhancement")
      && ok;

  ok = expect(!parsePkconLine("Transaction: Searching"), "status line") && ok;
  ok = expect(!parsePkconLine(""), "empty") && ok;

  const auto hits = parsePkconOutput(
      "Installed    foo-1.0-1.fc44.x86_64 (fedora)\n"
      "Available    foo-1.1-1.fc44.x86_64 (updates)\n"
      "Bug fix      foo-1.2-1.fc44.x86_64 (updates)\n"
      "Available    other-1-1.fc44.x86_64 (fedora)\n"
  );
  ok = expect(hits.size() == 4, "parsed four") && ok;
  const auto deduped = dedupeByName(hits);
  ok = expect(deduped.size() == 2, "deduped two names") && ok;
  ok = expect(!deduped.empty() && deduped.front().name == "foo" && deduped.front().state == PackageState::Update, "prefer update")
      && ok;

  auto ranked = parsePkconOutput(
      "Enhancement  zebra-1-1.fc44.x86_64 (updates)\n"
      "Security     PackageKit-1-1.fc44.x86_64 (updates)\n"
      "Security     alpha-1-1.fc44.x86_64 (updates)\n"
      "Bug fix      mid-1-1.fc44.x86_64 (updates)\n"
      "Installed    inst-1-1.fc44.x86_64 (fedora)\n"
  );
  sortHits(ranked);
  ok = expect(ranked.size() == 5, "ranked five") && ok;
  ok = expect(!ranked.empty() && ranked[0].name == "alpha", "security alpha first") && ok;
  ok = expect(ranked.size() > 1 && ranked[1].name == "PackageKit", "security name case") && ok;
  ok = expect(ranked.size() > 2 && ranked[2].name == "mid", "bug fix third") && ok;
  ok = expect(ranked.size() > 3 && ranked[3].name == "zebra", "enhancement fourth") && ok;
  ok = expect(ranked.size() > 4 && ranked[4].name == "inst", "installed last") && ok;

  PkconProgress progress;
  ok = expect(applyPkconProgressLine("Status:\tDownloading packages", progress), "status line") && ok;
  ok = expect(progress.status == "Downloading packages", "status text") && ok;
  ok = expect(applyPkconProgressLine("Percentage:    45", progress), "percent line") && ok;
  ok = expect(progress.percent == 45, "percent value") && ok;
  ok = expect(!applyPkconProgressLine("Installed    helix-1-1.fc44.x86_64 (fedora)", progress), "package line") && ok;

  ok = expect(compactSummary("  GNU  Image\nManipulation  ") == "GNU Image Manipulation", "compact summary") && ok;
  const auto rpmRow = parseNameSummaryLine("bash\tThe GNU Bourne Again shell", '\t');
  ok = expect(rpmRow && rpmRow->first == "bash" && rpmRow->second == "The GNU Bourne Again shell", "rpm summary") && ok;
  const auto dnfRow = parseNameSummaryLine("helix|A post-modern modal text editor written in Rust", '|');
  ok = expect(dnfRow && dnfRow->first == "helix", "dnf name") && ok;
  ok = expect(!parseNameSummaryLine("bad;name|nope", '|'), "invalid summary name") && ok;

  auto withSummary = parsePkconOutput("Available    helix-25.07.1-11.fc44.x86_64 (updates)\n");
  applySummaryMap(withSummary, parseNameSummaryOutput("helix|A post-modern modal text editor written in Rust\n", '|'));
  ok = expect(withSummary.size() == 1 && withSummary[0].summary.starts_with("A post-modern"), "apply summary") && ok;

  const auto rpmInstalled = parseRpmInstalledLine("bash\t0\t5.3.0\t2.fc44\tx86_64\tThe GNU Bourne Again shell");
  ok = expect(rpmInstalled && rpmInstalled->name == "bash" && rpmInstalled->epoch.empty(), "rpm name") && ok;
  ok = expect(rpmInstalled && rpmInstalled->version == "5.3.0" && rpmInstalled->release == "2.fc44", "rpm version")
      && ok;
  ok = expect(rpmInstalled && rpmInstalled->arch == "x86_64" && rpmInstalled->state == PackageState::Installed, "rpm arch")
      && ok;
  ok = expect(rpmInstalled && rpmInstalled->summary.starts_with("The GNU Bourne"), "rpm summary") && ok;

  const auto rpmEpoch = parseRpmInstalledLine("vim-data\t2\t9.2.1061\t1.fc44\tnoarch\tVi IMproved extra files");
  ok = expect(rpmEpoch && rpmEpoch->name == "vim-data" && rpmEpoch->epoch == "2", "rpm epoch") && ok;
  ok = expect(!parseRpmInstalledLine("Transaction: Searching"), "rpm junk") && ok;
  ok = expect(parseRpmInstalledOutput("bash\t0\t5.3.0\t2.fc44\tx86_64\tThe GNU Bourne Again shell\n").size() == 1, "rpm one")
      && ok;

  ok = expect(isValidFlatpakAppId("org.gnome.Calculator"), "flatpak id") && ok;
  ok = expect(isValidFlatpakAppId("com.github.user.App-Name"), "flatpak hyphen") && ok;
  ok = expect(!isValidFlatpakAppId("org"), "flatpak one component") && ok;
  ok = expect(!isValidFlatpakAppId("org.gnome."), "flatpak trailing dot") && ok;
  ok = expect(!isValidFlatpakAppId("../etc"), "flatpak path") && ok;
  ok = expect(preferredFlatpakRemote("fedora, flathub") == "flathub", "prefer flathub") && ok;
  ok = expect(preferredFlatpakRemote("fedora") == "fedora", "first remote") && ok;
  ok = expect(preferredFlatpakRemote("").empty(), "empty remotes") && ok;

  const auto flatpakInstalled = parseFlatpakListLine(
      "org.gnome.Calculator\tCalculator\t49.1.2\tstable\tflathub\tuser", FlatpakListKind::Installed
  );
  ok = expect(flatpakInstalled && flatpakInstalled->name == "org.gnome.Calculator", "flatpak app id") && ok;
  ok = expect(flatpakInstalled && flatpakInstalled->title == "Calculator" && flatpakInstalled->version == "49.1.2", "flatpak title")
      && ok;
  ok = expect(flatpakInstalled && flatpakInstalled->repo == "flathub" && flatpakInstalled->installation == "user", "flatpak origin")
      && ok;
  ok = expect(flatpakInstalled && flatpakInstalled->state == PackageState::Installed, "flatpak installed state") && ok;

  const auto flatpakUpdate = parseFlatpakListLine(
      "org.gnome.Calculator\tCalculator\tPerform arithmetic\t49.2\tflathub", FlatpakListKind::Updates
  );
  ok = expect(flatpakUpdate && flatpakUpdate->state == PackageState::Update && flatpakUpdate->summary.starts_with("Perform"), "flatpak update")
      && ok;
  ok = expect(!parseFlatpakListLine("runtime/org.freedesktop.Platform\tPlatform\t25.08\tstable\tflathub\tuser", FlatpakListKind::Installed), "runtime skipped")
      && ok;

  const auto searched = parseFlatpakSearchJson(
      R"([{"application_id":"org.gnome.Calculator","name":"Calculator","description":"Perform arithmetic","version":"49.0","remotes":"flathub,fedora"}])"
  );
  ok = expect(searched.size() == 1 && searched[0].name == "org.gnome.Calculator", "search id") && ok;
  ok = expect(searched.size() == 1 && searched[0].title == "Calculator" && searched[0].repo == "flathub,fedora", "search title")
      && ok;
  const auto searchedAlt = parseFlatpakSearchJson(R"([{"application":"org.mozilla.firefox","name":"Firefox"}])");
  ok = expect(searchedAlt.size() == 1 && searchedAlt[0].name == "org.mozilla.firefox", "search application key") && ok;
  ok = expect(parseFlatpakSearchJson(R"([{"application_id":"bad"}])").empty(), "search invalid id") && ok;

  ok = expect(isValidFwupdDeviceId("362301da643102b9f38477387e2193e57abaa590"), "fwupd id") && ok;
  ok = expect(!isValidFwupdDeviceId("short"), "fwupd short") && ok;
  ok = expect(!isValidFwupdDeviceId("362301da643102b9f38477387e2193e57abaa59g"), "fwupd nonhex") && ok;
  ok = expect(!isValidFwupdDeviceId("../etc/passwd"), "fwupd path") && ok;

  const auto fwUpdates = parseFwupdDevicesJson(
      R"({"Devices":[{"Name":"UEFI dbx","DeviceId":"362301da643102b9f38477387e2193e57abaa590","Summary":"UEFI revocation database","Vendor":"Microsoft","Version":"20260402","Flags":["updatable","supported"],"Releases":[{"Name":"Secure Boot dbx","Summary":"Forbidden Signature Database","Version":"20260707","RemoteId":"lvfs","Description":"<p>Updates dbx.</p>"}]}]})",
      FwupdListKind::Updates
  );
  ok = expect(fwUpdates.size() == 1 && fwUpdates[0].name == "362301da643102b9f38477387e2193e57abaa590", "fwupd update id")
      && ok;
  ok = expect(
           fwUpdates.size() == 1 && fwUpdates[0].title == "UEFI dbx" && fwUpdates[0].version == "20260707"
               && fwUpdates[0].state == PackageState::Update && fwUpdates[0].repo == "lvfs",
           "fwupd update fields"
       )
      && ok;
  ok = expect(
           fwUpdates.size() == 1 && fwUpdates[0].summary.starts_with("Forbidden"), "fwupd update summary"
       )
      && ok;

  const auto fwInstalled = parseFwupdDevicesJson(
      R"({"Devices":[{"Name":"Touchpad","DeviceId":"9ec2f35557ba426df27002d8b03f67b0d4d2e7a3","Summary":"Touch Controller","Vendor":"Goodix","Version":"0.68223","Flags":["updatable"]},{"Name":"Hub","DeviceId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","Flags":["internal"]}]})",
      FwupdListKind::Installed
  );
  ok = expect(fwInstalled.size() == 1 && fwInstalled[0].name == "9ec2f35557ba426df27002d8b03f67b0d4d2e7a3", "fwupd installed")
      && ok;
  ok = expect(
           fwInstalled.size() == 1 && fwInstalled[0].title == "Touchpad" && fwInstalled[0].version == "0.68223"
               && fwInstalled[0].state == PackageState::Installed,
           "fwupd installed fields"
       )
      && ok;
  ok = expect(parseFwupdDevicesJson(R"({"Devices":[]})", FwupdListKind::Updates).empty(), "fwupd empty") && ok;

  return ok ? 0 : 1;
}
