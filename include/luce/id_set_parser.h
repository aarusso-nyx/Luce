#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace luce {
namespace parse {

enum class IdSetError {
  kOk,
  kEmpty,
  kInvalidToken,
  kInvalidRange,
  kOutOfRange,
};

struct IdSetIssue {
  IdSetError error;
  char token[32];
  std::uint32_t bad_id;
};

inline std::size_t copy_token(char* out, std::size_t out_size, const char* src, std::size_t len) {
  if (out_size == 0) {
    return 0;
  }
  const std::size_t copy_len = (len + 1 < out_size) ? len : out_size - 1;
  if (src && copy_len > 0) {
    std::memcpy(out, src, copy_len);
  }
  out[copy_len] = '\0';
  return copy_len;
}

inline bool parse_decimal_uint32(const char* text, std::size_t len, std::uint32_t* out) {
  if (!text || len == 0 || !out) {
    return false;
  }
  char buffer[16] = {0};
  if (len + 1 > sizeof(buffer)) {
    return false;
  }
  std::memcpy(buffer, text, len);
  char* end_ptr = nullptr;
  const unsigned long value = std::strtoul(buffer, &end_ptr, 10);
  if (end_ptr == buffer || end_ptr == nullptr || *end_ptr != '\0') {
    return false;
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

// Visit each id in a comma + dash expression like "1,3-5,7".
// Calls visitor(id) for each id in [min_id, max_id]; if allow_all is true
// the literal token "all" expands to the whole inclusive range.
// Returns IdSetError::kOk on success and writes the applied count to
// *applied_count. On failure, fills *issue with the offending token text
// (best-effort, truncated to 31 chars) so callers can log it.
template <typename Visitor>
IdSetError parse_id_set(const char* expr,
                        std::uint32_t min_id,
                        std::uint32_t max_id,
                        bool allow_all,
                        std::uint16_t* applied_count,
                        Visitor visitor,
                        IdSetIssue* issue = nullptr) {
  if (applied_count) {
    *applied_count = 0;
  }
  if (issue) {
    issue->error = IdSetError::kOk;
    issue->token[0] = '\0';
    issue->bad_id = 0;
  }
  if (!expr || expr[0] == '\0') {
    if (issue) {
      issue->error = IdSetError::kEmpty;
    }
    return IdSetError::kEmpty;
  }

  std::size_t cursor = 0;
  const std::size_t expr_len = std::strlen(expr);
  bool any_token = false;
  while (cursor <= expr_len) {
    const char* comma = std::strchr(expr + cursor, ',');
    const std::size_t end_pos = comma ? static_cast<std::size_t>(comma - expr) : expr_len;
    const std::size_t token_len = end_pos - cursor;
    const char* token = expr + cursor;
    if (token_len > 0) {
      any_token = true;
      if (allow_all && token_len == 3 && std::strncmp(token, "all", 3) == 0) {
        for (std::uint32_t id = min_id; id <= max_id; ++id) {
          visitor(id);
          if (applied_count && *applied_count < UINT16_MAX) {
            ++(*applied_count);
          }
        }
      } else {
        const char* dash = static_cast<const char*>(std::memchr(token, '-', token_len));
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        if (!dash) {
          if (!parse_decimal_uint32(token, token_len, &lo)) {
            if (issue) {
              issue->error = IdSetError::kInvalidToken;
              copy_token(issue->token, sizeof(issue->token), token, token_len);
            }
            return IdSetError::kInvalidToken;
          }
          hi = lo;
        } else {
          const std::size_t left_len = static_cast<std::size_t>(dash - token);
          const std::size_t right_len = token_len - left_len - 1;
          if (left_len == 0 || right_len == 0) {
            if (issue) {
              issue->error = IdSetError::kInvalidRange;
              copy_token(issue->token, sizeof(issue->token), token, token_len);
            }
            return IdSetError::kInvalidRange;
          }
          if (!parse_decimal_uint32(token, left_len, &lo) ||
              !parse_decimal_uint32(dash + 1, right_len, &hi)) {
            if (issue) {
              issue->error = IdSetError::kInvalidRange;
              copy_token(issue->token, sizeof(issue->token), token, token_len);
            }
            return IdSetError::kInvalidRange;
          }
        }
        if (lo > hi) {
          const std::uint32_t swap_tmp = lo;
          lo = hi;
          hi = swap_tmp;
        }
        for (std::uint32_t id = lo; id <= hi; ++id) {
          if (id < min_id || id > max_id) {
            if (issue) {
              issue->error = IdSetError::kOutOfRange;
              issue->bad_id = id;
              copy_token(issue->token, sizeof(issue->token), token, token_len);
            }
            return IdSetError::kOutOfRange;
          }
          visitor(id);
          if (applied_count && *applied_count < UINT16_MAX) {
            ++(*applied_count);
          }
        }
      }
    }
    if (!comma) {
      break;
    }
    cursor = end_pos + 1;
  }
  if (!any_token) {
    if (issue) {
      issue->error = IdSetError::kEmpty;
    }
    return IdSetError::kEmpty;
  }
  return IdSetError::kOk;
}

}  // namespace parse
}  // namespace luce
