#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unity.h>
#include <errno.h>

namespace {

uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  return on ? static_cast<uint8_t>(current_mask | bit) : static_cast<uint8_t>(current_mask & ~bit);
}

uint8_t relay_mask_for_channel(int channel) {
  return static_cast<uint8_t>(1u << channel);
}

void cli_trim(char* line) {
  char* write = line;
  for (const char* read = line; *read != '\0'; ++read) {
    if (*read == '\r' || *read == '\n') {
      continue;
    }
    if (write != read) {
      *write = *read;
    }
    ++write;
  }
  *write = '\0';
}

bool parse_u32_with_base(const char* text, int base, uint32_t* value, char* token_context = nullptr) {
  if (!text || !*text || !value) {
    return false;
  }
  if (token_context) {
    std::strncpy(token_context, text, 31);
    token_context[31] = '\0';
  }

  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text, &end, base);
  if (errno != 0 || end == text || *end != '\0' || parsed > 0xFFFFFFFFULL) {
    return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

std::size_t tokenize_cli_line(char* line, char* argv[], std::size_t max_args) {
  std::size_t argc = 0;
  char* next_token = nullptr;
  char* token = strtok_r(line, " \t", &next_token);
  while (token && argc < max_args) {
    argv[argc++] = token;
    token = strtok_r(nullptr, " \t", &next_token);
  }
  return argc;
}

constexpr bool kRelayActiveHigh = false;
constexpr uint8_t kRelayOffValue = kRelayActiveHigh ? 0x00 : 0xFF;

uint8_t relay_mask_for_channel_state_active_low(int channel, bool on, uint8_t current_mask) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  return on ? static_cast<uint8_t>(current_mask & ~bit) : static_cast<uint8_t>(current_mask | bit);
}

void format_mask_line(char* out, std::size_t out_size, uint8_t relay_mask, uint8_t button_mask) {
  std::snprintf(out, out_size, "REL:0x%02X BTN:0x%02X", relay_mask, button_mask);
}

void test_bitmask_helpers_single_channel_state() {
  TEST_ASSERT_EQUAL_UINT8(0x01, relay_mask_for_channel(0));
  TEST_ASSERT_EQUAL_UINT8(0x80, relay_mask_for_channel(7));
}

void test_bitmask_helpers_set_clear_sequence() {
  uint8_t current = 0x00;
  current = relay_mask_for_channel_state(2, true, current);
  TEST_ASSERT_EQUAL_UINT8(0x04, current);
  current = relay_mask_for_channel_state(2, false, current);
  TEST_ASSERT_EQUAL_UINT8(0x00, current);

  current = relay_mask_for_channel_state(1, true, current);
  current = relay_mask_for_channel_state(4, true, current);
  TEST_ASSERT_EQUAL_UINT8(0x12, current);
}

void test_bitmask_helpers_active_low_semantics() {
  TEST_ASSERT_TRUE(kRelayActiveHigh == false);
  TEST_ASSERT_EQUAL_UINT8(0xFF, kRelayOffValue);

  uint8_t current = kRelayOffValue;
  current = relay_mask_for_channel_state_active_low(2, true, current);
  TEST_ASSERT_EQUAL_UINT8(0xFB, current);
  current = relay_mask_for_channel_state_active_low(2, false, current);
  TEST_ASSERT_EQUAL_UINT8(0xFF, current);

  current = relay_mask_for_channel_state_active_low(7, true, 0xFF);
  TEST_ASSERT_EQUAL_UINT8(0x7F, current);
}

void test_parse_u32_with_base_decimal() {
  uint32_t value = 0;
  TEST_ASSERT_TRUE(parse_u32_with_base("42", 10, &value, nullptr));
  TEST_ASSERT_EQUAL_UINT32(42, value);
  TEST_ASSERT_TRUE(parse_u32_with_base("0", 10, &value, nullptr));
  TEST_ASSERT_EQUAL_UINT32(0, value);
  TEST_ASSERT_TRUE(parse_u32_with_base("4294967295", 10, &value, nullptr));
  TEST_ASSERT_EQUAL_UINT32(4294967295u, value);
}

void test_parse_u32_with_base_hex() {
  uint32_t value = 0;
  TEST_ASSERT_TRUE(parse_u32_with_base("ff", 16, &value, nullptr));
  TEST_ASSERT_EQUAL_UINT32(0xFFu, value);
  TEST_ASSERT_TRUE(parse_u32_with_base("0x1A", 16, &value, nullptr));
  TEST_ASSERT_EQUAL_UINT32(0x1Au, value);
  char token[32] = {0};
  TEST_ASSERT_FALSE(parse_u32_with_base("12ab", 10, &value, token));
  TEST_ASSERT_EQUAL_STRING("12ab", token);
  TEST_ASSERT_FALSE(parse_u32_with_base("4294967296", 10, &value, token));
}

void test_cli_trim_and_tokenize() {
  char raw[] = " relay_mask \t ff \r\n";
  cli_trim(raw);
  TEST_ASSERT_EQUAL_STRING(" relay_mask \t ff ", raw);

  char* argv[8];
  std::size_t argc = tokenize_cli_line(raw, argv, 8);
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(argc));
  TEST_ASSERT_EQUAL_STRING("relay_mask", argv[0]);
  TEST_ASSERT_EQUAL_STRING("ff", argv[1]);
}

void test_tokenization_stable_empty_and_spacing() {
  char raw[] = "   status   \t   \t";
  cli_trim(raw);
  char* argv[8];
  const std::size_t argc = tokenize_cli_line(raw, argv, 8);
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(argc));
  TEST_ASSERT_EQUAL_STRING("status", argv[0]);

  char none[] = "\r\n";
  cli_trim(none);
  char* argv2[8];
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(tokenize_cli_line(none, argv2, 8)));
}

void test_formatting_helpers_stable_hex() {
  char out[32];
  format_mask_line(out, sizeof(out), 0x00, 0xAB);
  TEST_ASSERT_EQUAL_STRING("REL:0x00 BTN:0xAB", out);
  format_mask_line(out, sizeof(out), 0xF0, 0x01);
  TEST_ASSERT_EQUAL_STRING("REL:0xF0 BTN:0x01", out);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_bitmask_helpers_single_channel_state);
  RUN_TEST(test_bitmask_helpers_set_clear_sequence);
  RUN_TEST(test_bitmask_helpers_active_low_semantics);
  RUN_TEST(test_parse_u32_with_base_decimal);
  RUN_TEST(test_parse_u32_with_base_hex);
  RUN_TEST(test_cli_trim_and_tokenize);
  RUN_TEST(test_tokenization_stable_empty_and_spacing);
  RUN_TEST(test_formatting_helpers_stable_hex);
  return UNITY_END();
}
