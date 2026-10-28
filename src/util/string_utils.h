#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace StringUtils {

  [[nodiscard]] inline std::string_view trimLeftView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
      s.remove_prefix(1);
    }
    return s;
  }

  [[nodiscard]] inline std::string_view trimRightView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
      s.remove_suffix(1);
    }
    return s;
  }

  [[nodiscard]] inline std::string trim(std::string_view s) { return std::string(trimRightView(trimLeftView(s))); }

  [[nodiscard]] inline std::string toLower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

  inline void toLowerInPlace(std::string& s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }

  [[nodiscard]] inline std::string utf32ToUtf8(std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
      return std::string(1, static_cast<char>(codepoint));
    }
    if (codepoint <= 0x7FFU) {
      return {static_cast<char>(0xC0U | (codepoint >> 6U)), static_cast<char>(0x80U | (codepoint & 0x3FU))};
    }
    if (codepoint <= 0xFFFFU) {
      return {
          static_cast<char>(0xE0U | (codepoint >> 12U)), static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)),
          static_cast<char>(0x80U | (codepoint & 0x3FU))
      };
    }
    if (codepoint <= 0x10FFFFU) {
      return {
          static_cast<char>(0xF0U | (codepoint >> 18U)), static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)),
          static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)), static_cast<char>(0x80U | (codepoint & 0x3FU))
      };
    }
    return {};
  }

  [[nodiscard]] inline bool isSinglePrivateUseGlyph(std::string_view text) {
    if (text.empty()) {
      return false;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    std::size_t len = 0;
    char32_t cp = 0;
    if (lead < 0x80) {
      len = 1;
      cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      len = 2;
      cp = lead & 0x1FU;
    } else if ((lead & 0xF0) == 0xE0) {
      len = 3;
      cp = lead & 0x0FU;
    } else if ((lead & 0xF8) == 0xF0) {
      len = 4;
      cp = lead & 0x07U;
    } else {
      return false;
    }
    if (text.size() != len) {
      return false; // more than one codepoint, or a truncated sequence
    }
    for (std::size_t i = 1; i < len; ++i) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if ((byte & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (byte & 0x3FU);
    }
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD);
  }

  [[nodiscard]] inline std::string truncateUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
      return std::string(text);
    }

    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return std::string(text.substr(0, end));
  }

} // namespace StringUtils
