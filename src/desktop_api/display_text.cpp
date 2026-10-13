#include "desktop_api/display_text.h"

#include <cstdint>
#include <filesystem>

namespace helix::agent {

  namespace {

    // Number of bytes in the UTF-8 sequence starting at `lead`, or 0 if `lead` cannot start one.
    std::size_t sequenceLength(unsigned char lead) {
      if (lead < 0x80)
        return 1;
      if (lead >= 0xC2 && lead <= 0xDF)
        return 2;
      if (lead >= 0xE0 && lead <= 0xEF)
        return 3;
      if (lead >= 0xF0 && lead <= 0xF4)
        return 4;
      return 0;
    }

    std::uint32_t decode(const std::string& text, std::size_t at, std::size_t length) {
      std::uint32_t value = length == 2 ? (static_cast<unsigned char>(text[at]) & 0x1F)
          : length == 3                 ? (static_cast<unsigned char>(text[at]) & 0x0F)
                                        : (static_cast<unsigned char>(text[at]) & 0x07);
      for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80)
          return 0xFFFFFFFF;
        value = (value << 6) | (next & 0x3F);
      }
      return value;
    }

    bool isHiddenFormatting(std::uint32_t cp) {
      return (cp >= 0x200B && cp <= 0x200F)
          || (cp >= 0x202A && cp <= 0x202E)
          || (cp >= 0x2060 && cp <= 0x2064)
          || (cp >= 0x2066 && cp <= 0x2069)
          || cp == 0xFEFF
          || (cp >= 0x80 && cp < 0xA0)
          || cp == 0x2028
          || cp == 0x2029;
    }

  } // namespace

  std::string sanitizeForDisplay(const std::string& text, std::size_t maxCodepoints) {
    std::string out;
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
      const auto lead = static_cast<unsigned char>(text[i]);
      std::size_t length = sequenceLength(lead);
      std::uint32_t cp = 0xFFFFFFFF;
      if (length != 0 && i + length <= text.size()) {
        cp = length == 1 ? lead : decode(text, i, length);
        // Overlong forms, surrogates and out-of-range values are how hostile text hides from filters.
        const bool overlong = (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000);
        if (cp != 0xFFFFFFFF && (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)))
          cp = 0xFFFFFFFF;
      } else {
        length = 1;
      }

      std::string piece;
      if (cp == 0xFFFFFFFF) {
        piece = "?"; // invalid UTF-8
      } else if (cp == '\n' || cp == '\r' || cp == '\t') {
        piece = " ";
      } else if (cp < 0x20 || cp == 0x7F || isHiddenFormatting(cp)) {
        piece.clear();
      } else {
        piece = text.substr(i, length);
      }
      i += length;
      if (piece.empty())
        continue;
      if (count == maxCodepoints) {
        out += "…";
        return out;
      }
      out += piece;
      ++count;
    }
    return out;
  }

  std::string friendlyClientName(const std::string& client, std::size_t maxChars) {
    // The script for interpreter-run clients, otherwise the program.
    const auto hash = client.find('#');
    std::string path =
        hash != std::string::npos ? client.substr(hash + 1) : (client.starts_with("exe:") ? client.substr(4) : client);
    // Take the last path component by hand: std::filesystem would treat control bytes in the name as ordinary text.
    const auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string out;
    for (const unsigned char ch : name) {
      if (out.size() == maxChars) {
        out += "...";
        break;
      }
      const bool plain = (ch >= 'a' && ch <= 'z')
          || (ch >= 'A' && ch <= 'Z')
          || (ch >= '0' && ch <= '9')
          || ch == '.'
          || ch == '_'
          || ch == '+'
          || ch == '-';
      out += plain ? static_cast<char>(ch) : '_';
    }
    return out.empty() ? "unknown" : out;
  }

} // namespace helix::agent
