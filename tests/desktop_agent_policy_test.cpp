#include "desktop/agent_policy.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace helix::agent;

namespace {

std::string slurp(const std::filesystem::path& file) {
  std::ifstream in(file);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

mode_t permissions(const std::filesystem::path& file) {
  struct stat info{};
  TEST_CHECK(::stat(file.c_str(), &info) == 0);
  return info.st_mode & 0777;
}

constexpr const char* kPolicy = R"(
[tiers]
status = "observe"
clipboard-text = "observe-private"
panel-open = "act"
app-launch = "confirm"
session = "blocked"
[choices]
"session lock" = "act"
)";

void checkTiers() {
  TEST_CHECK(parseTier("observe-private") == Tier::ObservePrivate);
  TEST_CHECK(!parseTier("admin").has_value());
  TEST_CHECK(tierName(Tier::Confirm) == "confirm");

  auto policy = RiskPolicy::parse(kPolicy);
  TEST_CHECK(policy.has_value());
  TEST_CHECK(policy->tierFor("status") == Tier::Observe);
  TEST_CHECK(policy->tierFor("app-launch") == Tier::Confirm);
  TEST_CHECK(policy->tierFor("not-a-command") == Tier::Blocked);  // unknown fails closed
  const std::vector<std::string> lock{"lock"}, reboot{"reboot"};
  TEST_CHECK(policy->tierFor("session", lock) == Tier::Act);
  TEST_CHECK(policy->tierFor("session", reboot) == Tier::Blocked);
  TEST_CHECK(policy->tierFor("status", lock) == Tier::Observe);  // overrides only apply to their command

  TEST_CHECK(!RiskPolicy::parse("[tiers]\nstatus = \"godmode\"\n").has_value());
  TEST_CHECK(!RiskPolicy::parse("[tiers\n").has_value());
  TEST_CHECK(!RiskPolicy::parse("").has_value());
}

void checkRealPolicy(const std::filesystem::path& root) {
  auto policy = RiskPolicy::load(root / "protocols/ipc/msg-risk.toml");
  TEST_CHECK(policy.has_value());
  TEST_CHECK(policy->commands().size() > 100);
  const std::vector<std::string> reboot{"reboot"}, lock{"lock"};
  TEST_CHECK(policy->tierFor("session", reboot) == Tier::Blocked);
  TEST_CHECK(policy->tierFor("session", lock) == Tier::Act);
  TEST_CHECK(policy->tierFor("firewall-panic-toggle") == Tier::Blocked);
  TEST_CHECK(policy->tierFor("clipboard-text") == Tier::ObservePrivate);
  TEST_CHECK(!RiskPolicy::load(root / "does-not-exist.toml").has_value());
}

void checkGrants(const std::filesystem::path& dir) {
  const auto file = dir / "state" / "grants.json";
  GrantStore store(file);
  TEST_CHECK(store.load().has_value());  // missing file is fine
  TEST_CHECK(!store.allows("exe:/usr/bin/agent", Tier::Observe));

  store.set("exe:/usr/bin/agent", {Tier::Observe, Tier::Act, Tier::Blocked});
  TEST_CHECK(store.allows("exe:/usr/bin/agent", Tier::Act));
  TEST_CHECK(!store.allows("exe:/usr/bin/agent", Tier::Confirm));
  TEST_CHECK(!store.allows("exe:/usr/bin/agent", Tier::Blocked));  // never grantable
  TEST_CHECK(store.tiersFor("exe:/usr/bin/agent").size() == 2);
  TEST_CHECK(store.save().has_value());
  TEST_CHECK(permissions(file) == 0600);

  GrantStore reloaded(file);
  TEST_CHECK(reloaded.load().has_value());
  TEST_CHECK(reloaded.allows("exe:/usr/bin/agent", Tier::Observe));
  TEST_CHECK(!reloaded.allows("exe:/usr/bin/other", Tier::Observe));

  // A hand-edited file cannot grant `blocked`.
  { std::ofstream out(file); out << R"({"version":1,"clients":{"x":["act","blocked"]}})"; }
  GrantStore edited(file);
  TEST_CHECK(edited.load().has_value());
  TEST_CHECK(edited.allows("x", Tier::Act) && !edited.allows("x", Tier::Blocked));

  // Corrupt files grant nothing and report an error.
  { std::ofstream out(file); out << "{not json"; }
  GrantStore corrupt(file);
  corrupt.set("y", {Tier::Act});
  TEST_CHECK(!corrupt.load().has_value());
  TEST_CHECK(!corrupt.allows("y", Tier::Act));

  reloaded.revoke("exe:/usr/bin/agent");
  TEST_CHECK(reloaded.clients().empty());
}

void checkEvaluate() {
  auto policy = RiskPolicy::parse(kPolicy);
  TEST_CHECK(policy.has_value());
  GrantStore grants(std::filesystem::path("/nonexistent/grants.json"));
  TaintTracker taint;
  const std::string client = "exe:/usr/bin/agent";
  const std::vector<std::string> noArgs, reboot{"reboot"};

  auto denied = evaluate(*policy, grants, taint, client, "status", noArgs);
  TEST_CHECK(denied.verdict == Verdict::Deny && denied.reason.find("no observe grant") != std::string::npos);

  grants.set(client, {Tier::Observe, Tier::ObservePrivate, Tier::Act, Tier::Confirm});
  TEST_CHECK(evaluate(*policy, grants, taint, client, "panel-open", noArgs).verdict == Verdict::Allow);
  TEST_CHECK(evaluate(*policy, grants, taint, client, "app-launch", noArgs).verdict == Verdict::NeedsConfirmation);
  TEST_CHECK(evaluate(*policy, grants, taint, client, "session", reboot).verdict == Verdict::Deny);
  TEST_CHECK(evaluate(*policy, grants, taint, client, "bogus", noArgs).reason == "unknown command");
  TEST_CHECK(evaluate(*policy, grants, taint, "exe:/other", "status", noArgs).verdict == Verdict::Deny);

  // Reading private content taints the client until a human confirms.
  TEST_CHECK(resultIsUntrusted(Tier::ObservePrivate) && !resultIsUntrusted(Tier::Observe));
  TEST_CHECK(!evaluate(*policy, grants, taint, client, "app-launch", noArgs).tainted);
  taint.markUntrusted(client);
  TEST_CHECK(evaluate(*policy, grants, taint, client, "app-launch", noArgs).tainted);
  taint.clear(client);
  TEST_CHECK(!taint.tainted(client));
}

void checkAudit(const std::filesystem::path& dir) {
  const auto file = dir / "audit" / "agent-audit.jsonl";
  AuditLog log(file);
  AuditRecord record;
  record.client = "exe:/usr/bin/agent";
  record.tool = "clipboard-copy";
  record.args = {std::string(500, 'a'), "line\nbreak \"quoted\""};
  record.tier = Tier::Confirm;
  record.decision = "confirmed";
  record.tainted = true;
  TEST_CHECK(log.append(record, std::chrono::system_clock::time_point{}).has_value());
  record.decision = "denied";
  TEST_CHECK(log.append(record).has_value());
  TEST_CHECK(permissions(file) == 0600);

  std::ifstream in(file);
  std::string first, second, extra;
  TEST_CHECK(std::getline(in, first) && std::getline(in, second) && !std::getline(in, extra));  // one record per line
  const auto parsed = nlohmann::json::parse(first);
  TEST_CHECK(parsed["time"] == "1970-01-01T00:00:00Z");
  TEST_CHECK(parsed["tier"] == "confirm" && parsed["decision"] == "confirmed" && parsed["tainted"] == true);
  TEST_CHECK(parsed["args"][0].get<std::string>().size() <= AuditLog::kMaxArgBytes + 4);
  TEST_CHECK(parsed["args"][1] == "line\nbreak \"quoted\"");  // survives as JSON, not as a second log line
}

}  // namespace

int main(int argc, char** argv) {
  TEST_CHECK(argc >= 2);
  const auto scratch = std::filesystem::temp_directory_path() / ("helix-agent-policy-" + std::to_string(::getpid()));
  std::filesystem::create_directories(scratch);
  checkTiers();
  checkRealPolicy(argv[1]);
  checkGrants(scratch);
  checkEvaluate();
  checkAudit(scratch);
  std::filesystem::remove_all(scratch);
  return 0;
}
