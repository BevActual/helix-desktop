// One-shot theme template renderer for Rust `helix-desktop` (no session D-Bus).
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "net/http_client.h"
#include "theme/template_apply_service.h"
#include "theme/theme_service.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

  void drainDeferredCalls() {
    for (auto callback : DeferredCall::takePending()) {
      if (callback) {
        callback();
      }
    }
  }

} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: helix-template-apply\n"
                   "Resolve the current theme palette and run TemplateEngine for enabled templates.\n";
      return 0;
    }
    std::cerr << "error: unknown argument: " << arg << '\n';
    return 1;
  }

  ConfigService config;
  HttpClient http;
  helix::theme::ThemeService theme(config, http);
  helix::theme::TemplateApplyService templates(config);

  theme.setResolvedCallback([&](const helix::theme::GeneratedPalette& palette, std::string_view mode) {
    templates.apply(palette, mode, true, true);
  });

  theme.apply();

  templates.waitUntilIdle();
  drainDeferredCalls();

  const auto failures = templates.lastFailures();
  if (!failures.empty()) {
    for (const auto& [id, error] : failures) {
      std::cerr << "template failure: " << id << ": " << error << '\n';
    }
    return 2;
  }
  return 0;
}
