#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {

  [[nodiscard]] std::filesystem::path configPath();
  [[nodiscard]] std::filesystem::path includePath(std::string_view fileName);

  [[nodiscard]] std::string readFile(const std::filesystem::path& path);
  [[nodiscard]] bool writeFile(const std::filesystem::path& path, std::string_view contents, std::string& error);

  [[nodiscard]] bool isIdent(char c) noexcept;
  [[nodiscard]] std::string stripComments(std::string_view in);
  [[nodiscard]] std::optional<std::string> parseQuoted(std::string_view text, std::size_t& i);
  [[nodiscard]] std::string fileNameOf(std::string_view path);

  [[nodiscard]] std::vector<std::filesystem::path>
  parseIncludes(std::string_view kdl, const std::filesystem::path& fromFile);
  [[nodiscard]] bool hasInclude(std::string_view kdl, std::string_view fileName);
  [[nodiscard]] bool
  appendInclude(const std::filesystem::path& configFile, std::string_view fileName, std::string& error);

  // Concatenate config.kdl and followed includes (comments stripped) for detection.
  [[nodiscard]] std::string collectConfigText(const std::filesystem::path& path);

} // namespace compositors::niri
