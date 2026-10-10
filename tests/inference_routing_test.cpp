#include "inference/routing.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string readFixture(const fs::path& root) {
  std::ifstream in(root / "tests/fixtures/inference-routing-serde-sample.toml");
  assert(in);
  std::ostringstream body;
  body << in.rdbuf();
  return body.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: inference_routing_test <repo-root>\n";
    return 1;
  }

  const fs::path repoRoot = argv[1];
  const fs::path stateHome = fs::temp_directory_path() / "helix-inference-routing-test-state";
  const fs::path helixDir = stateHome / "helix";
  fs::create_directories(helixDir);
  {
    std::ofstream out(helixDir / "inference-routing.toml");
    out << readFixture(repoRoot);
  }
  if (setenv("HELIX_STATE_HOME", stateHome.c_str(), 1) != 0) {
    std::cerr << "setenv failed\n";
    return 1;
  }

  const helix::inference::RoutingTable routing = helix::inference::loadRouting();
  assert(routing.route("act").model == "qwen-act-q4.gguf");
  assert(routing.route("chat").model == "qwen-chat-q4.gguf");
  assert(routing.route("code").backend == "lan");
  assert(routing.route("code").model == "codellama");
  assert(!routing.isLocal("code"));
  assert(routing.isLocal("act"));
  assert(routing.allowRemoteOnBatterySaver);
  bool foundLan = false;
  for (const auto& backend : routing.backends) {
    if (backend.id == "lan") {
      foundLan = true;
      assert(backend.kind == "openai");
      assert(backend.baseUrl == "http://127.0.0.1:11434/v1");
    }
  }
  assert(foundLan);
  std::cout << "inference_routing_test: all checks passed\n";
  return 0;
}
