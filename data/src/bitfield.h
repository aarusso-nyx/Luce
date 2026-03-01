// bitfield.h: template-based bitfield abstraction for mask manipulations
#pragma once

#include <cstddef>
#include <type_traits>
#include <string>

// Template bitfield for up to N bits stored in integral type T
template<typename T, std::size_t N>
class BitField {
  static_assert(std::is_integral<T>::value, "BitField requires integral type");
  static_assert(N <= sizeof(T) * 8, "BitField size exceeds underlying type capacity");
public:
  using value_type = T;

  constexpr BitField() : mask_(0) {}
  constexpr BitField(T mask) : mask_(mask) {}

  // Get or set full mask
  constexpr T get() const { return mask_; }
  void set(T mask) { mask_ = mask; }

  // Get or set individual bit at index [0..N)
  constexpr bool get(std::size_t idx) const { return (mask_ >> idx) & static_cast<T>(1); }
  void set(std::size_t idx, bool value) {
    if (value) mask_ |= (static_cast<T>(1) << idx);
    else        mask_ &= ~(static_cast<T>(1) << idx);
  }
  // Toggle bit at index
  void toggle(std::size_t idx) { mask_ ^= (static_cast<T>(1) << idx); }

  // Comparisons
  constexpr bool operator==(const BitField& other) const { return mask_ == other.mask_; }
  constexpr bool operator!=(const BitField& other) const { return !(*this == other); }

  // Implicit conversion to underlying mask
  constexpr operator T() const { return mask_; }
  
  /**
   * Convert bitfield to string of '0'/'1' characters, MSB first.
   * If spaced is true, inserts a space after every 8 bits for readability.
   */
  std::string toString(bool spaced = false) const {
    std::string s;
    // Reserve space: N bits plus spaces between groups of 8
    size_t groups = spaced ? (N + 7) / 8 - 1 : 0;
    s.reserve(N + groups);
    size_t printed = 0;
    for (size_t i = 0; i < N; ++i) {
      // bit index from MSB (N-1) down to 0
      size_t bit = N - 1 - i;
      s.push_back(((mask_ >> bit) & static_cast<T>(1)) ? '1' : '0');
      ++printed;
      if (spaced && printed % 8 == 0 && i + 1 < N) {
        s.push_back(' ');
      }
    }
    return s;
  }
  
  /**
   * Parse a bit pattern string (e.g. "1010 0101") into a BitField.
   * Returns true on success; spaced or unspaced strings of exactly N bits are accepted.
   */
  static bool fromString(const char* str, BitField& out) {
    T m = 0;
    size_t count = 0;
    for (const char* p = str; *p != '\0'; ++p) {
      if (*p == ' ') {
        continue;
      } else if (*p == '0' || *p == '1') {
        // Shift in next bit
        m = static_cast<T>((m << 1) | static_cast<T>(*p - '0'));
        ++count;
        if (count > N) {
          return false;
        }
      } else {
        // Invalid character
        return false;
      }
    }
    if (count != N) {
      return false;
    }
    out.mask_ = m;
    return true;
  }
  /**
   * Convenience overload accepting std::string
   */
  static bool fromString(const std::string& str, BitField& out) {
    return fromString(str.c_str(), out);
  }

private:
  T mask_;
};

// end of header