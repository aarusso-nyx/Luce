#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace luce {
namespace json {

class Writer {
public:
  Writer(char* buffer, std::size_t size) : buffer_(buffer), size_(size) {
    if (buffer_ != nullptr && size_ > 0) {
      buffer_[0] = '\0';
    } else {
      truncated_ = true;
    }
  }

  bool begin_object() {
    if (!append_value_prefix()) {
      return false;
    }
    if (!append_char('{')) {
      return false;
    }
    push_scope();
    return !truncated_;
  }

  bool key_object_begin(const char* key) {
    if (!append_key_prefix(key)) {
      return false;
    }
    if (!append_char('{')) {
      return false;
    }
    push_scope();
    return !truncated_;
  }

  bool end_object() {
    if (depth_ == 0) {
      truncated_ = true;
      return false;
    }
    --depth_;
    return append_char('}');
  }

  bool key_str(const char* key, const char* value) {
    return append_key_prefix(key) && append_quoted(value != nullptr ? value : "");
  }

  bool key_int(const char* key, std::int32_t value) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%ld", static_cast<long>(value));
    return append_key_prefix(key) && append_raw(text);
  }

  bool key_uint(const char* key, std::uint32_t value) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
    return append_key_prefix(key) && append_raw(text);
  }

  bool key_bool(const char* key, bool value) {
    return append_key_prefix(key) && append_raw(value ? "true" : "false");
  }

  bool key_double(const char* key, double value, int precision = 3) {
    char format[8] = {};
    char text[32] = {};
    if (precision < 0) {
      precision = 0;
    } else if (precision > 9) {
      precision = 9;
    }
    std::snprintf(format, sizeof(format), "%%.%df", precision);
    std::snprintf(text, sizeof(text), format, value);
    return append_key_prefix(key) && append_raw(text);
  }

  bool truncated() const { return truncated_; }
  const char* c_str() const { return buffer_ != nullptr ? buffer_ : ""; }
  std::size_t size() const { return used_; }

private:
  static constexpr std::size_t kMaxDepth = 8;

  bool append_value_prefix() {
    if (depth_ > 0) {
      if (needs_comma_[depth_ - 1] && !append_char(',')) {
        return false;
      }
      needs_comma_[depth_ - 1] = true;
    }
    return true;
  }

  bool append_key_prefix(const char* key) {
    if (key == nullptr || depth_ == 0) {
      truncated_ = true;
      return false;
    }
    if (!append_value_prefix()) {
      return false;
    }
    return append_quoted(key) && append_char(':');
  }

  void push_scope() {
    if (depth_ >= kMaxDepth) {
      truncated_ = true;
      return;
    }
    needs_comma_[depth_++] = false;
  }

  bool append_char(char ch) {
    if (buffer_ == nullptr || size_ == 0) {
      truncated_ = true;
      return false;
    }
    if (used_ + 1 >= size_) {
      truncated_ = true;
      buffer_[size_ - 1] = '\0';
      return false;
    }
    buffer_[used_++] = ch;
    buffer_[used_] = '\0';
    return true;
  }

  bool append_raw(const char* text) {
    if (text == nullptr) {
      return true;
    }
    while (*text != '\0') {
      if (!append_char(*text++)) {
        return false;
      }
    }
    return true;
  }

  bool append_hex_escape(unsigned char ch) {
    constexpr char kHex[] = "0123456789ABCDEF";
    return append_raw("\\u00") && append_char(kHex[(ch >> 4) & 0x0Fu]) &&
           append_char(kHex[ch & 0x0Fu]);
  }

  bool append_quoted(const char* text) {
    if (!append_char('"')) {
      return false;
    }
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(text != nullptr ? text : "");
         *p != '\0'; ++p) {
      switch (*p) {
      case '"':
        if (!append_raw("\\\"")) {
          return false;
        }
        break;
      case '\\':
        if (!append_raw("\\\\")) {
          return false;
        }
        break;
      case '\b':
        if (!append_raw("\\b")) {
          return false;
        }
        break;
      case '\f':
        if (!append_raw("\\f")) {
          return false;
        }
        break;
      case '\n':
        if (!append_raw("\\n")) {
          return false;
        }
        break;
      case '\r':
        if (!append_raw("\\r")) {
          return false;
        }
        break;
      case '\t':
        if (!append_raw("\\t")) {
          return false;
        }
        break;
      default:
        if (*p < 0x20u) {
          if (!append_hex_escape(*p)) {
            return false;
          }
        } else if (!append_char(static_cast<char>(*p))) {
          return false;
        }
        break;
      }
    }
    return append_char('"');
  }

  char* buffer_ = nullptr;
  std::size_t size_ = 0;
  std::size_t used_ = 0;
  bool truncated_ = false;
  bool needs_comma_[kMaxDepth] = {};
  std::size_t depth_ = 0;
};

} // namespace json
} // namespace luce
