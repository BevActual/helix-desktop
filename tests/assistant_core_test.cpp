#include "assistant/assistant_core.h"
#include "tests/test_check.h"

#include <filesystem>

using namespace helix::assistant;
using helix::agent::Tier;

int main(int argc, char** argv) {
  TEST_CHECK(argc >= 2);
  auto policy = helix::agent::RiskPolicy::load(std::filesystem::path(argv[1]) / "protocols/ipc/msg-risk.toml");
  TEST_CHECK(policy.has_value());
  const std::set<Tier> everyday{Tier::Observe, Tier::ObservePrivate, Tier::Act};

  // Replies are read strictly.
  const auto call = parseReply(R"({"tool":"panel-open","args":["control-center"],"reason":"you asked"})");
  TEST_CHECK(call.kind == Reply::Kind::Call && call.tool == "panel-open" && call.args.size() == 1 && call.reason == "you asked");
  TEST_CHECK(parseReply("not json").kind == Reply::Kind::Invalid);
  TEST_CHECK(parseReply(R"({"args":[]})").kind == Reply::Kind::Invalid);
  TEST_CHECK(parseReply(R"({"tool":"status","args":[1]})").kind == Reply::Kind::Invalid);
  TEST_CHECK(parseReply(R"({"tool":"status","args":"x"})").kind == Reply::Kind::Invalid);
  TEST_CHECK(parseReply("[]").kind == Reply::Kind::Invalid);

  // A decline is never a call, and cannot smuggle arguments through.
  const auto decline = parseReply(R"({"tool":"decline","args":["session","reboot"],"reason":"no tool for that"})");
  TEST_CHECK(decline.kind == Reply::Kind::Decline && decline.args.empty() && decline.reason == "no tool for that");
  TEST_CHECK(stepFor(*policy, everyday, decline) == Step::Explain);
  TEST_CHECK(stepFor(*policy, everyday, parseReply("junk")) == Step::Explain);

  // Reading runs straight away; changing something is shown first.
  TEST_CHECK(stepFor(*policy, everyday, parseReply(R"({"tool":"status","args":[]})")) == Step::Run);
  TEST_CHECK(stepFor(*policy, everyday, parseReply(R"({"tool":"volume-up","args":[]})")) == Step::AskFirst);
  TEST_CHECK(stepFor(*policy, everyday, parseReply(R"({"tool":"session","args":["lock"]})")) == Step::AskFirst);

  // Whatever the model says, tools outside the grants are refused here as well as by the service.
  TEST_CHECK(stepFor(*policy, everyday, parseReply(R"({"tool":"session","args":["reboot"]})")) == Step::Refuse);
  TEST_CHECK(stepFor(*policy, everyday, parseReply(R"({"tool":"made-up","args":[]})")) == Step::Refuse);
  TEST_CHECK(stepFor(*policy, {Tier::Observe}, parseReply(R"({"tool":"volume-up","args":[]})")) == Step::Refuse);
  TEST_CHECK(stepFor(*policy, {Tier::Observe}, parseReply(R"({"tool":"status","args":[]})")) == Step::Run);
  TEST_CHECK(stepFor(*policy, {}, parseReply(R"({"tool":"status","args":[]})")) == Step::Refuse);

  // What the person is shown is the exact call.
  TEST_CHECK(describe(call) == "Run `panel-open control-center`");
  const auto hints = helix::agent::ToolHints::parse("[hints]\npower-cycle = \"Step to the next power profile\"\n");
  TEST_CHECK(hints.has_value());
  const auto restart = parseReply(R"({"tool":"power-cycle","args":["next"]})");
  TEST_CHECK(describe(restart, &*hints) == "Step to the next power profile (`power-cycle next`)");
  TEST_CHECK(describe(call, &*hints) == "Run `panel-open control-center`");  // no hint: the exact call is still shown

  // The prompt carries the tool list, the examples, and the request last, verbatim.
  const auto messages = buildMessages("- status\n", "Turn it up.");
  TEST_CHECK(messages.front().at("role") == "system" && messages.front().at("content").get<std::string>().find("- status") != std::string::npos);
  TEST_CHECK(messages.back().at("role") == "user" && messages.back().at("content") == "Turn it up.");
  TEST_CHECK(messages.size() == 2 + 2 * 5);

  // Model choice: the person's choice, else the largest.
  const nlohmann::json models = nlohmann::json::array({{{"id", "small.gguf"}, {"bytes", 1000}}, {{"id", "big.gguf"}, {"bytes", 9000}}});
  TEST_CHECK(chooseModel(models, "").id == "big.gguf");
  TEST_CHECK(chooseModel(models, "small.gguf").id == "small.gguf");
  TEST_CHECK(!chooseModel(models, "missing.gguf").error.empty() && chooseModel(models, "missing.gguf").id.empty());
  TEST_CHECK(!chooseModel(nlohmann::json::array(), "").error.empty());
  TEST_CHECK(!chooseModel(nlohmann::json{{"x", 1}}, "").error.empty());
  return 0;
}
