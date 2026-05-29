#include "luce/str_utils.h"

#include <cstdint>
#include <initializer_list>

#include "third_party/minitest.h"

void test_str_utils() {
  MINITEST_EXPECT_TRUE(luce::str::starts_with("abcdef", "abc"));
  MINITEST_EXPECT_FALSE(luce::str::starts_with("abcdef", "abd"));

  char text[] = " \t hello \r\n";
  MINITEST_EXPECT_STREQ(luce::str::trim_ascii_inplace(text), "hello");

  bool flag = false;
  for (const char* token : {"on", "true", "1", "yes", "ON"}) {
    flag = false;
    MINITEST_EXPECT_TRUE(luce::str::parse_bool_token(token, &flag));
    MINITEST_EXPECT_TRUE(flag);
  }
  for (const char* token : {"off", "false", "0", "no", "OFF"}) {
    flag = true;
    MINITEST_EXPECT_TRUE(luce::str::parse_bool_token(token, &flag));
    MINITEST_EXPECT_FALSE(flag);
  }
  MINITEST_EXPECT_FALSE(luce::str::parse_bool_token("maybe", &flag));

  std::uint32_t value = 0;
  MINITEST_EXPECT_TRUE(luce::str::parse_u32_token("42", &value, 10));
  MINITEST_EXPECT_EQ(value, 42u);
  MINITEST_EXPECT_TRUE(luce::str::parse_u32_token("0x2a", &value, 0));
  MINITEST_EXPECT_EQ(value, 42u);
  MINITEST_EXPECT_TRUE(luce::str::parse_u32_token("2a", &value, 16));
  MINITEST_EXPECT_EQ(value, 42u);
  MINITEST_EXPECT_FALSE(luce::str::parse_u32_token("42x", &value, 10));
  MINITEST_EXPECT_FALSE(luce::str::parse_u32_token("4294967296", &value, 10));
}
