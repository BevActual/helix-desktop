#include "compositors/niri/niri_config_files.h"

#include "util/file_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace compositors::niri {
  namespace {

    [[nodiscard]] std::filesystem::path xdgConfigHome() {
      if (const char* env = std::getenv("XDG_CONFIG_HOME"); env != nullptr && env[0] != '\0') {
        return std::filesystem::path(env);
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config";
      }
      return {};
    }

    [[nodiscard]] std::filesystem::path resolveInclude(std::string_view raw, const std::filesystem::path& fromFile) {
      std::filesystem::path path = FileUtils::expandUserPath(std::string(raw));
      if (path.is_relative()) {
        path = fromFile.parent_path() / path;
      }
      return path.lexically_normal();
    }

    void collectRecursive(
        const std::filesystem::path& path, std::string& out, std::unordered_set<std::string>& visited, int depth
    ) {
      if (depth > 16 || path.empty()) {
        return;
      }
      std::error_code ec;
      const auto canonical = std::filesystem::weakly_canonical(path, ec);
      const std::string key = (!ec ? canonical : path).string();
      if (!visited.insert(key).second) {
        return;
      }
      const std::string text = readFile(path);
      if (text.empty() && !std::filesystem::exists(path, ec)) {
        return;
      }
      if (!out.empty()) {
        out.push_back('\n');
      }
      out += stripComments(text);
      for (const auto& include : parseIncludes(text, path)) {
        collectRecursive(include, out, visited, depth + 1);
      }
    }

  } // namespace

  std::filesystem::path configPath() {
    if (const char* env = std::getenv("NIRI_CONFIG"); env != nullptr && env[0] != '\0') {
      return FileUtils::expandUserPath(env);
    }
    const auto home = xdgConfigHome();
    if (home.empty()) {
      return {};
    }
    return home / "niri" / "config.kdl";
  }

  std::filesystem::path includePath(std::string_view fileName) {
    return configPath().parent_path() / std::string(fileName);
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  bool writeFile(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec && readFile(path) == contents) {
      return true;
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        error = "failed to create " + parent.string();
        return false;
      }
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      error = "failed to write " + path.string();
      return false;
    }
    out << contents;
    if (!out) {
      error = "failed to write " + path.string();
      return false;
    }
    return true;
  }

  bool isIdent(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '+';
  }

  std::string stripComments(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inString = false;
    bool escape = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
      const char c = in[i];
      if (inString) {
        out.push_back(c);
        if (escape) {
          escape = false;
          continue;
        }
        if (c == '\\') {
          escape = true;
          continue;
        }
        if (c == '"') {
          inString = false;
        }
        continue;
      }
      if (c == '"') {
        inString = true;
        out.push_back(c);
        continue;
      }
      if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
        while (i < in.size() && in[i] != '\n') {
          ++i;
        }
        if (i < in.size()) {
          out.push_back('\n');
        }
        continue;
      }
      out.push_back(c);
    }
    return out;
  }

  std::optional<std::string> parseQuoted(std::string_view text, std::size_t& i) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
    if (i >= text.size() || text[i] != '"') {
      return std::nullopt;
    }
    ++i;
    std::string value;
    bool escape = false;
    for (; i < text.size(); ++i) {
      const char c = text[i];
      if (escape) {
        value.push_back(c);
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') {
        ++i;
        return value;
      }
      value.push_back(c);
    }
    return std::nullopt;
  }

  std::string fileNameOf(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) {
      return std::string(path);
    }
    return std::string(path.substr(slash + 1));
  }

  std::vector<std::filesystem::path> parseIncludes(std::string_view kdl, const std::filesystem::path& fromFile) {
    const std::string stripped = stripComments(kdl);
    std::vector<std::filesystem::path> includes;
    std::size_t i = 0;
    while (i < stripped.size()) {
      const auto found = stripped.find("include", i);
      if (found == std::string::npos) {
        break;
      }
      if (found > 0 && isIdent(stripped[found - 1])) {
        i = found + 7;
        continue;
      }
      if (found + 7 < stripped.size() && isIdent(stripped[found + 7])) {
        i = found + 7;
        continue;
      }
      std::size_t cursor = found + 7;
      if (const auto quoted = parseQuoted(stripped, cursor); quoted.has_value()) {
        includes.push_back(resolveInclude(*quoted, fromFile));
        i = cursor;
      } else {
        i = found + 7;
      }
    }
    return includes;
  }

  bool hasInclude(std::string_view kdl, std::string_view fileName) {
    const std::string stripped = stripComments(kdl);
    const auto includes = parseIncludes(stripped, "dummy.kdl");
    return std::ranges::any_of(includes, [&](const std::filesystem::path& include) {
      return include.filename() == fileName || fileNameOf(include.string()) == fileName;
    });
  }

  bool appendInclude(const std::filesystem::path& configFile, std::string_view fileName, std::string& error) {
    const std::string includeLine = std::string("include \"") + std::string(fileName) + "\"\n";
    std::error_code ec;
    if (!std::filesystem::exists(configFile, ec) || ec) {
      error = "niri config not found";
      return false;
    }
    const std::string existing = readFile(configFile);
    if (hasInclude(existing, fileName)) {
      return true;
    }
    std::ofstream out(configFile, std::ios::app);
    if (!out) {
      error = "failed to update " + configFile.string();
      return false;
    }
    if (!existing.empty() && existing.back() != '\n') {
      out << '\n';
    }
    out << '\n' << includeLine;
    if (!out) {
      error = "failed to update " + configFile.string();
      return false;
    }
    return true;
  }

  std::string collectConfigText(const std::filesystem::path& path) {
    std::string out;
    std::unordered_set<std::string> visited;
    collectRecursive(path, out, visited, 0);
    return out;
  }

} // namespace compositors::niri
