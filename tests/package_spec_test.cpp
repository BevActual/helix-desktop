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

  return ok ? 0 : 1;
}
