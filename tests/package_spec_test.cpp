#include "system/package_spec.h"

#include <algorithm>
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
  using namespace helix::packages;
  bool ok = true;

  ok = expect(isValidRpmName("helix"), "plain name") && ok;
  ok = expect(isValidRpmName("libstdc++"), "plus plus") && ok;
  ok = expect(isValidRpmName("pkg-name.1_2"), "dotted") && ok;
  ok = expect(!isValidRpmName(""), "empty name") && ok;
  ok = expect(!isValidRpmName("-leading"), "leading dash") && ok;
  ok = expect(!isValidRpmName("bad;name"), "metachar") && ok;

  {
    PackageHit update{
        .name = "helix",
        .version = "5.2.1",
        .release = "1.fc44",
        .arch = "x86_64",
        .repo = "updates",
    };
    ok = expect(
        pkconPackageId(update) == "helix;5.2.1-1.fc44;x86_64;updates", "packagekit id"
    ) && ok;
    ok = expect(isValidPkconPackageId(pkconPackageId(update)), "packagekit id valid") && ok;
    PackageHit epoch{
        .name = "vim-data",
        .version = "9.2.1061",
        .release = "1.fc44",
        .arch = "noarch",
        .epoch = "2",
        .repo = "updates",
    };
    ok = expect(
        pkconPackageId(epoch) == "vim-data;2:9.2.1061-1.fc44;noarch;updates", "epoch packagekit id"
    ) && ok;
    PackageHit nameOnly{.name = "htop"};
    ok = expect(pkconPackageId(nameOnly).empty(), "incomplete id empty") && ok;
    ok = expect(pkconPackageRef(nameOnly) == "htop", "name fallback ref") && ok;
    ok = expect(isValidPkconPackageRef("helix;5.2.1-1.fc44;x86_64;updates"), "id ref") && ok;
    ok = expect(isValidPkconPackageRef("htop"), "name ref") && ok;
    ok = expect(!isValidPkconPackageRef("bad;"), "bad ref") && ok;
    PackageHit caret{
        .name = "helix-nightly",
        .version = "5.2.1^20261004git.e639a87",
        .release = "1.fc44",
        .arch = "x86_64",
        .repo = "terra",
    };
    ok = expect(
        pkconPackageId(caret) == "helix-nightly;5.2.1^20261004git.e639a87-1.fc44;x86_64;terra",
        "caret version id"
    ) && ok;
    ok = expect(
        pkconPackageRefForAction(update, false) == "helix;5.2.1-1.fc44;x86_64;updates", "action install id"
    ) && ok;
    ok = expect(
        pkconPackageRefForAction(update, true) == "helix;5.2.1-1.fc44;x86_64;installed:updates",
        "action remove id"
    ) && ok;
    PackageHit alreadyInstalled = update;
    alreadyInstalled.repo = "installed:updates";
    ok = expect(
        pkconPackageRefForAction(alreadyInstalled, true) == "helix;5.2.1-1.fc44;x86_64;installed:updates",
        "remove keeps installed data"
    ) && ok;
  }

  ok = expect(isSafeSearchQuery("ht"), "min length") && ok;
  ok = expect(isSafeSearchQuery("helix shell"), "space") && ok;
  ok = expect(!isSafeSearchQuery("x"), "too short") && ok;
  ok = expect(!isSafeSearchQuery("foo; rm"), "semicolon") && ok;
  ok = expect(!isSafeSearchQuery("*"), "glob") && ok;
  ok = expect(hasSafeSearchChars("h"), "one char") && ok;
  ok = expect(!hasSafeSearchChars("h;"), "bad char") && ok;

  ok = expect(isProtectedPackage("helix-greeter"), "greeter") && ok;
  ok = expect(isProtectedPackage("helix-shell"), "helix-shell") && ok;
  ok = expect(isProtectedPackage("filesystem"), "essential filesystem protected") && ok;
  ok = expect(isProtectedPackage("systemd"), "session manager protected") && ok;
  ok = expect(isProtectedPackage("helix-niri"), "active compositor protected") && ok;
  ok = expect(isProtectedPackage("bash"), "shell protected") && ok;
  ok = expect(!isProtectedPackage("systemd-devel"), "development package removable") && ok;

  ok = expect(!isProtectedPackage("helix"), "editor") && ok;
  ok = expect(isRpmDevelPackage("glibc-devel"), "devel class") && ok;
  ok = expect(isRpmLibraryPackage("libpng"), "lib prefix class") && ok;
  ok = expect(isRpmLibraryPackage("gtk4-libs"), "libs suffix class") && ok;
  ok = expect(!isRpmLibraryPackage("libreoffice"), "libreoffice stays app") && ok;
  ok = expect(isRpmAppPackage("cowsay"), "app class") && ok;
  ok = expect(isRpmAppPackage("htop"), "htop app") && ok;
  ok = expect(matchesRpmScope("cowsay", "apps"), "apps scope") && ok;
  ok = expect(!matchesRpmScope("glibc-devel", "apps"), "devel not apps") && ok;
  ok = expect(matchesRpmScope("glibc-devel", "devel"), "devel scope") && ok;

  std::vector<PackageHit> rankedSearch = {
      {.name = "htop-devel", .title = "htop-devel", .summary = "Headers", .state = PackageState::Available},
      {.name = "libhtop", .title = "libhtop", .summary = "Library", .state = PackageState::Available},
      {.name = "htop", .title = "htop", .summary = "Interactive process viewer", .state = PackageState::Available},
      {.name = "python3-htop", .title = "python3-htop", .summary = "Bindings", .state = PackageState::Available},
  };
  rankRpmSearchHits("htop", rankedSearch);
  ok = expect(!rankedSearch.empty() && rankedSearch[0].name == "htop", "rpm search ranks exact first") && ok;

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

  PkconProgress flatpakProgress;
  ok = expect(applyFlatpakProgressLine("Installing 2/2… ███▏                  16%", flatpakProgress), "flatpak percent")
      && ok;
  ok = expect(flatpakProgress.percent == 16, "flatpak percent value") && ok;
  ok = expect(flatpakProgress.status.find("Installing 2/2") != std::string::npos, "flatpak status") && ok;
  ok = expect(applyFlatpakProgressLine("Installation complete.", flatpakProgress), "flatpak complete") && ok;
  ok = expect(flatpakProgress.percent == 100, "flatpak complete percent") && ok;
  ok = expect(!applyFlatpakProgressLine("org.gnome.Calculator permissions:", flatpakProgress), "flatpak noise") && ok;

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
  const auto flatpakCatalog = parseFlatpakListLine(
      "org.gnome.Calculator\tCalculator\tPerform arithmetic\t49.2\tflathub", FlatpakListKind::Catalog
  );
  ok = expect(
           flatpakCatalog && flatpakCatalog->state == PackageState::Available
               && flatpakCatalog->summary.starts_with("Perform"),
           "flatpak catalog"
       )
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
  ok = expect(
           fwInstalled.size() == 1 && fwInstalled[0].iconName.empty(), "fwupd installed no icon"
       )
      && ok;
  const auto fwWithIcon = parseFwupdDevicesJson(
      R"({"Devices":[{"Name":"Touchpad","DeviceId":"9ec2f35557ba426df27002d8b03f67b0d4d2e7a3","Summary":"Touch Controller","Vendor":"Goodix","Version":"0.68223","Flags":["updatable"],"Icons":["input-touchpad"]}]})",
      FwupdListKind::Installed
  );
  ok = expect(fwWithIcon.size() == 1 && fwWithIcon[0].iconName == "input-touchpad", "fwupd icon name") && ok;

  const auto component = R"(<component type="desktop-application">
    <id>org.example.App</id>
    <name>Example</name>
    <summary>Short summary</summary>
    <project_license>GPL-3.0</project_license>
    <developer_name>Example Labs</developer_name>
    <description><p>Long description text.</p><ul><li>One</li></ul></description>
    <url type="homepage">https://example.org</url>
    <url type="bugtracker">https://example.org/issues</url>
    <url type="donation">https://example.org/donate</url>
    <url type="help">https://example.org/help</url>
    <content_rating type="oars-1.1">
      <content_attribute id="violence-cartoon">none</content_attribute>
      <content_attribute id="drugs-alcohol">mild</content_attribute>
    </content_rating>
    <category>Utility</category>
    <category>Development</category>
    <custom>
      <value key="flathub::verification::verified">true</value>
      <value key="flathub::verification::method">website</value>
      <value key="flathub::verification::website">example.org</value>
    </custom>
    <releases>
      <release version="1.2.3">
        <description><p>Fixed crash on launch and improved search.</p></description>
      </release>
    </releases>
    <screenshots>
      <screenshot type="default">
        <image>https://dl.flathub.org/repo/screenshots/org.example.App-stable/752x423/one.png</image>
        <image>https://dl.flathub.org/repo/screenshots/org.example.App-stable/112x63/one.png</image>
      </screenshot>
    </screenshots>
  </component>)";
  const auto extracted = extractAppstreamComponentXml(
      std::string("<components>") + component + "<component><id>other.app</id></component></components>",
      "org.example.App"
  );
  ok = expect(extracted && extracted->contains("<id>org.example.App</id>"), "extract component") && ok;
  ok = expect(!extractAppstreamComponentXml("<components/>", "org.example.App"), "missing component") && ok;

  PackageHit base{.name = "org.example.App"};
  auto details = parseAppstreamComponentXml(component, base);
  ok = expect(details.hit.title == "Example" && details.hit.summary == "Short summary", "appstream title") && ok;
  ok = expect(details.license == "GPL-3.0" && details.developer == "Example Labs", "appstream license") && ok;
  ok = expect(details.homepage == "https://example.org", "appstream homepage") && ok;
  ok = expect(details.bugtracker == "https://example.org/issues", "appstream bugtracker") && ok;
  ok = expect(details.donation == "https://example.org/donate", "appstream donation") && ok;
  ok = expect(details.helpUrl == "https://example.org/help", "appstream help") && ok;
  ok = expect(details.contentRating == "May include mild content", "appstream content rating") && ok;
  ok = expect(details.releaseNotes.contains("1.2.3") && details.releaseNotes.contains("Fixed crash"), "appstream release notes")
      && ok;
  ok = expect(details.verification.contains("example.org"), "appstream verification") && ok;
  ok = expect(details.description.contains("Long description"), "appstream description") && ok;
  ok = expect(details.categories.size() == 2 && details.categories[0] == "Utility", "appstream categories") && ok;
  ok = expect(
           !details.screenshotUrls.empty()
               && details.screenshotUrls.front().contains("752x423"),
           "appstream screenshot prefers larger"
       )
      && ok;

  auto remoteInfo = parseFlatpakRemoteInfo(
      "Example App\n\n            ID: org.example.App\n       Version: 1.2.3\n       License: MIT\n Download Size: 12.0 MB\nInstalled Size: 40.0 MB\n       Runtime: org.freedesktop.Platform/x86_64/25.08\n",
      PackageHit{.name = "org.example.App"}
  );
  ok = expect(remoteInfo.hit.version == "1.2.3" && remoteInfo.license == "MIT", "remote info version") && ok;
  ok = expect(remoteInfo.downloadSize == "12.0 MB" && remoteInfo.runtime.contains("Platform"), "remote info sizes")
      && ok;

  auto permissions = parseFlatpakPermissions("[Context]\nshared=network;ipc;\nsockets=x11;\n", {});
  ok = expect(permissions.permissions.contains("Has network access"), "permissions text") && ok;

  auto rpmInfo = parseRpmInfoOutput(
      "Name        : firefox\nVersion     : 157.0\nRelease     : 1.fc44\nArchitecture: x86_64\nSize        : 304740343\nLicense     : MPL-2.0\nURL         : https://www.mozilla.org/firefox/\nSummary     : Mozilla Firefox Web browser\nDescription :\nMozilla Firefox is an open-source web browser.\n",
      PackageHit{.name = "firefox"}
  );
  ok = expect(rpmInfo.hit.summary.starts_with("Mozilla Firefox"), "rpm info summary") && ok;
  ok = expect(rpmInfo.license == "MPL-2.0" && rpmInfo.homepage.contains("mozilla"), "rpm info license") && ok;
  ok = expect(rpmInfo.description.contains("open-source"), "rpm info description") && ok;
  ok = expect(rpmInfo.installedSize == "304740343", "rpm info size") && ok;

  auto fwDetails = enrichFromFwupdDeviceJson(
      R"({"Devices":[{"Name":"UEFI dbx","DeviceId":"362301da643102b9f38477387e2193e57abaa590","Summary":"UEFI revocation database","Vendor":"Microsoft","Version":"20260402","Icons":["computer"],"Releases":[{"Version":"20260707","Description":"<p>Updates dbx.</p>"}]}]})",
      PackageHit{.name = "362301da643102b9f38477387e2193e57abaa590"}
  );
  ok = expect(fwDetails.developer == "Microsoft" && fwDetails.hit.iconName == "computer", "fwupd detail vendor") && ok;
  ok = expect(fwDetails.description.contains("Updates dbx"), "fwupd detail notes") && ok;
  ok = expect(fwDetails.hit.version == "20260707", "fwupd detail version") && ok;

  ok = expect(plainDescription("  a\n\nb  ").contains("a"), "plain description") && ok;
  ok = expect(plainDescription("protect what&apos;s important").contains("what's"), "plain description entities")
      && ok;

  const auto permissionLabels = humanizeFlatpakPermissions(
      "[Context]\nshared=network;ipc;\nsockets=x11;wayland;pulseaudio;\ndevices=dri;\nfilesystems=home;xdg-download;\n"
  );
  ok = expect(std::ranges::find(permissionLabels, "Has network access") != permissionLabels.end(), "perm network") && ok;
  ok = expect(std::ranges::find(permissionLabels, "Can use GPU acceleration") != permissionLabels.end(), "perm dri") && ok;
  ok = expect(
           std::ranges::find(permissionLabels, "Can read and write your home folder") != permissionLabels.end(),
           "perm home"
       )
      && ok;
  ok = expect(categoryDisplayName("AudioVideo") == "Media", "category display") && ok;
  ok = expect(categoryDisplayName("Game") == "Games", "category games") && ok;

  PackageHit categorized{.name = "org.example.App"};
  std::vector<PackageHit> categorizedHits{categorized};
  attachFlatpakCategories(categorizedHits, {{"org.example.App", {"Game", "ActionGame"}}});

  ok = expect(isValidFlatpakRemoteName("flathub"), "remote name") && ok;
  ok = expect(!isValidFlatpakRemoteName(""), "empty remote") && ok;
  const auto remotes = parseFlatpakRemotesOutput(
      "flathub\tFlathub\thttps://dl.flathub.org/repo/\tuser\t1\n"
      "fedora-testing\tFedora Flatpaks (testing)\toci+https://example\tsystem,disabled,oci\t1\n"
  );
  ok = expect(remotes.size() == 2, "two remotes") && ok;
  ok = expect(!remotes.empty() && remotes[0].name == "flathub" && remotes[0].installation == "user", "user flathub")
      && ok;
  ok = expect(remotes.size() > 1 && remotes[1].disabled && remotes[1].oci, "disabled oci remote") && ok;
  ok = expect(hasUserFlathubRemote(remotes), "has user flathub") && ok;

  FlatpakCatalogIndex meta;
  meta.categories = {{"org.example.Verified", {"Utility"}}};
  meta.verified = {"org.example.Verified"};
  std::vector<PackageHit> verifiedHits{{.name = "org.example.Verified"}};
  attachFlatpakCatalogIndex(verifiedHits, meta);
  ok = expect(!verifiedHits.empty() && verifiedHits.front().verified, "verified attach") && ok;
  ok = expect(!verifiedHits.front().categories.empty(), "verified categories attach") && ok;
  ok = expect(categorizedHits[0].categories.size() == 2 && categorizedHits[0].categories[0] == "Game", "attach categories")
      && ok;

  return ok ? 0 : 1;
}
