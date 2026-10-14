// Renders templates with the C++ TemplateEngine for helix-theme parity goldens.
// Built and linked by tools/regen_golden_template.sh against build-debug/libhelix_core.a.
#include "core/toml.h"
#include "system/icon_resolver.h"
#include "theme/builtin_palettes.h"
#include "theme/template_engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace helix::theme;
namespace fs = std::filesystem;

namespace {
  std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    return h;
  }

  constexpr const char* kConfig = R"(
[shell]
name = "helix_shell"
size = 12
ratio = 1.5
enabled = true
list = ["a", "b", "c"]
[shell.nested]
k = "v"
)";
} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <assets/templates> <synthetic.tpl> [full-output-dir]\n", argv[0]);
    return 1;
  }
  const fs::path templatesDir = argv[1];
  const std::string synthetic = readFile(argv[2]);
  const fs::path fullDir = argc > 3 ? fs::path(argv[3]) : fs::path();

  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(templatesDir))
    if (e.is_regular_file())
      files.push_back(fs::relative(e.path(), templatesDir));
  std::sort(files.begin(), files.end());

  auto configTable = std::make_shared<toml::table>(toml::parse(kConfig));
  std::printf("icon_theme\t%s\n", IconResolver::activeThemeName().c_str());

  for (const auto& palette : builtinPalettes()) {
    const auto themeData = TemplateEngine::makeThemeData(expandBuiltinPalette(palette));
    for (const char* mode : {"dark", "light"}) {
      TemplateEngine::Options options;
      options.defaultMode = mode;
      options.imagePath = "/img/wall paper.png";
      options.closestColor = "#112233";
      options.configDir = "/home/u/.config/helix";
      options.configFile = "/home/u/.config/helix/config.toml";
      options.configTable = configTable;
      options.verbose = false;

      auto emit = [&](const std::string& name, const std::string& text, int errors) {
        std::printf(
            "%s\t%s\t%s\t%d\t%016llx\n", std::string(palette.name).c_str(), mode, name.c_str(), errors,
            static_cast<unsigned long long>(fnv1a(text))
        );
        if (!fullDir.empty()) {
          fs::create_directories(fullDir);
          std::ofstream out(fullDir / (std::string(palette.name) + "_" + mode + "_" + name));
          out << text;
        }
      };

      {
        TemplateEngine engine(themeData, options);
        const auto r = engine.render(synthetic);
        emit("synthetic.tpl", r.text, r.errorCount);
      }
      for (const auto& rel : files) {
        TemplateEngine engine(themeData, options);
        const auto r = engine.render(readFile(templatesDir / rel));
        std::string flat = rel.string();
        std::replace(flat.begin(), flat.end(), '/', '_');
        emit("t:" + rel.string(), r.text, r.errorCount);
      }
    }
  }
}
