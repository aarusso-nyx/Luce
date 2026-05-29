#pragma once

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace luce {
namespace str {

inline bool starts_with(const char* text, const char* prefix) {
  if (text == nullptr || prefix == nullptr) {
    return false;
  }
  while (*prefix != '\0') {
    if (*text++ != *prefix++) {
      return false;
    }
  }
  return true;
}

inline char* trim_ascii_inplace(char* text) {
  if (text == nullptr) {
    return nullptr;
  }
  std::size_t len = std::strlen(text);
  while (len > 0 && std::isspace(static_cast<unsigned char>(text[len - 1])) != 0) {
    text[--len] = '\0';
  }

  std::size_t start = 0;
  while (text[start] != '\0' && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  if (start > 0) {
    std::memmove(text, text + start, std::strlen(text + start) + 1);
  }
  return text;
}

inline char ascii_lower(char ch) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

inline bool ascii_iequals(const char* lhs, const char* rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  while (*lhs != '\0' && *rhs != '\0') {
    if (ascii_lower(*lhs++) != ascii_lower(*rhs++)) {
      return false;
    }
  }
  return *lhs == '\0' && *rhs == '\0';
}

inline bool parse_bool_token(const char* text, bool* out_value) {
  if (text == nullptr || out_value == nullptr || text[0] == '\0') {
    return false;
  }
  if (std::strcmp(text, "1") == 0 || ascii_iequals(text, "on") || ascii_iequals(text, "true") ||
      ascii_iequals(text, "yes")) {
    *out_value = true;
    return true;
  }
  if (std::strcmp(text, "0") == 0 || ascii_iequals(text, "off") || ascii_iequals(text, "false") ||
      ascii_iequals(text, "no")) {
    *out_value = false;
    return true;
  }
  return false;
}

inline bool parse_u32_token(const char* text, std::uint32_t* out_value, int base = 0) {
  if (text == nullptr || out_value == nullptr || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, base);
  if (errno != 0 || end == text || end == nullptr || *end != '\0' || parsed > 0xFFFFFFFFUL) {
    return false;
  }
  *out_value = static_cast<std::uint32_t>(parsed);
  return true;
}

} // namespace str
} // namespace luce
