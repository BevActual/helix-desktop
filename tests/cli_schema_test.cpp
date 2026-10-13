#include "cli/schema_msg.h"
#include "tests/test_check.h"
#include "theme/builtin_palettes.h"
#include "theme/scheme.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace {

  const helix::cli::Command& child(const helix::cli::Command& command, std::string_view name) {
    const auto it = std::ranges::find(command.subcommands, name, &helix::cli::Command::name);
    TEST_CHECK(it != command.subcommands.end());
    return *it;
  }

} // namespace

int main() {
  const helix::cli::Command* colorSchemeSet = helix::cli::findMsgCommand("color-scheme-set");
  TEST_CHECK(colorSchemeSet != nullptr);

  const auto& builtin = child(*colorSchemeSet, "builtin");
  TEST_CHECK(builtin.positionals.size() == 1);
  const auto palettes = helix::theme::builtinPalettes();
  TEST_CHECK(builtin.positionals.front().choices.size() == palettes.size());
  for (std::size_t i = 0; i < palettes.size(); ++i)
    TEST_CHECK(builtin.positionals.front().choices[i] == palettes[i].name);

  const auto& wallpaper = child(*colorSchemeSet, "wallpaper");
  TEST_CHECK(wallpaper.positionals.size() == 1);
  TEST_CHECK(wallpaper.positionals.front().choices.size() == helix::theme::kSchemeNames.size());
  for (std::size_t i = 0; i < helix::theme::kSchemeNames.size(); ++i) {
    const std::string_view name = helix::theme::kSchemeNames[i];
    TEST_CHECK(wallpaper.positionals.front().choices[i] == name);
    const auto parsed = helix::theme::schemeFromString(name);
    TEST_CHECK(parsed.has_value());
    TEST_CHECK(helix::theme::schemeToString(*parsed) == name);
  }

  return EXIT_SUCCESS;
}
