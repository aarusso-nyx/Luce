#include "luce/json_writer.h"

#include <cstring>

#include "third_party/minitest.h"

void test_json_writer() {
  {
    char buffer[256] = {};
    luce::json::Writer writer(buffer, sizeof(buffer));
    MINITEST_EXPECT_TRUE(writer.begin_object());
    MINITEST_EXPECT_TRUE(writer.key_str("quote", "a\"b"));
    MINITEST_EXPECT_TRUE(writer.key_str("slash", "a\\b"));
    MINITEST_EXPECT_TRUE(writer.key_str("line", "a\nb"));
    const char control[] = {'x', static_cast<char>(0x01), 'y', '\0'};
    MINITEST_EXPECT_TRUE(writer.key_str("control", control));
    MINITEST_EXPECT_TRUE(writer.key_int("i", -7));
    MINITEST_EXPECT_TRUE(writer.key_uint("u", 42));
    MINITEST_EXPECT_TRUE(writer.key_double("d", 1.25, 2));
    MINITEST_EXPECT_TRUE(writer.key_bool("b", true));
    MINITEST_EXPECT_TRUE(writer.end_object());
    MINITEST_EXPECT_FALSE(writer.truncated());
    MINITEST_EXPECT_STREQ(buffer,
                          "{\"quote\":\"a\\\"b\",\"slash\":\"a\\\\b\",\"line\":\"a\\nb\","
                          "\"control\":\"x\\u0001y\",\"i\":-7,\"u\":42,\"d\":1.25,\"b\":true}");
  }
  {
    char buffer[96] = {};
    luce::json::Writer writer(buffer, sizeof(buffer));
    MINITEST_EXPECT_TRUE(writer.begin_object());
    MINITEST_EXPECT_TRUE(writer.key_str("url", "https://example.com/fw?name=\"prod\""));
    MINITEST_EXPECT_TRUE(writer.end_object());
    MINITEST_EXPECT_FALSE(writer.truncated());
    MINITEST_EXPECT_TRUE(std::strstr(buffer, "\\\"prod\\\"") != nullptr);
  }
  {
    char buffer[12] = {};
    luce::json::Writer writer(buffer, sizeof(buffer));
    MINITEST_EXPECT_TRUE(writer.begin_object());
    MINITEST_EXPECT_FALSE(writer.key_str("long", "abcdef"));
    MINITEST_EXPECT_TRUE(writer.truncated());
    MINITEST_EXPECT_EQ(buffer[sizeof(buffer) - 1], '\0');
  }
}
