#include "luce/id_set_parser.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "third_party/minitest.h"

namespace {

struct Capture {
  std::vector<std::uint32_t> ids;

  void operator()(std::uint32_t id) {
    ids.push_back(id);
  }
};

}  // namespace

void test_id_set_parser() {
  using luce::parse::IdSetError;
  using luce::parse::IdSetIssue;

  {
    Capture capture;
    std::uint16_t applied = 0;
    IdSetIssue issue {};
    const auto result = luce::parse::parse_id_set("3", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); }, &issue);
    MINITEST_EXPECT_TRUE(result == IdSetError::kOk);
    MINITEST_EXPECT_EQ(applied, static_cast<std::uint16_t>(1));
    MINITEST_EXPECT_EQ(capture.ids.size(), static_cast<std::size_t>(1));
    MINITEST_EXPECT_EQ(capture.ids[0], 3u);
  }
  {
    Capture capture;
    std::uint16_t applied = 0;
    const auto result = luce::parse::parse_id_set("1,3-5,7", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); });
    MINITEST_EXPECT_TRUE(result == IdSetError::kOk);
    MINITEST_EXPECT_EQ(applied, static_cast<std::uint16_t>(5));
    MINITEST_EXPECT_EQ(capture.ids[0], 1u);
    MINITEST_EXPECT_EQ(capture.ids[1], 3u);
    MINITEST_EXPECT_EQ(capture.ids[4], 7u);
  }
  {
    Capture capture;
    std::uint16_t applied = 0;
    const auto result = luce::parse::parse_id_set("5-3", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); });
    MINITEST_EXPECT_TRUE(result == IdSetError::kOk);
    MINITEST_EXPECT_EQ(applied, static_cast<std::uint16_t>(3));
    MINITEST_EXPECT_EQ(capture.ids[0], 3u);
    MINITEST_EXPECT_EQ(capture.ids[2], 5u);
  }
  {
    Capture capture;
    std::uint16_t applied = 0;
    const auto result = luce::parse::parse_id_set("all", 2, 4, true, &applied, [&](std::uint32_t id) { capture(id); });
    MINITEST_EXPECT_TRUE(result == IdSetError::kOk);
    MINITEST_EXPECT_EQ(applied, static_cast<std::uint16_t>(3));
    MINITEST_EXPECT_EQ(capture.ids[0], 2u);
    MINITEST_EXPECT_EQ(capture.ids[2], 4u);
  }
  {
    Capture capture;
    std::uint16_t applied = 0;
    IdSetIssue issue {};
    const auto result = luce::parse::parse_id_set("all", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); }, &issue);
    MINITEST_EXPECT_TRUE(result == IdSetError::kInvalidToken);
    MINITEST_EXPECT_EQ(applied, static_cast<std::uint16_t>(0));
    MINITEST_EXPECT_STREQ(issue.token, "all");
  }
  {
    Capture capture;
    IdSetIssue issue {};
    std::uint16_t applied = 0;
    const auto result = luce::parse::parse_id_set("9", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); }, &issue);
    MINITEST_EXPECT_TRUE(result == IdSetError::kOutOfRange);
    MINITEST_EXPECT_EQ(issue.bad_id, 9u);
  }
  {
    Capture capture;
    IdSetIssue issue {};
    std::uint16_t applied = 0;
    const auto result = luce::parse::parse_id_set("x", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); }, &issue);
    MINITEST_EXPECT_TRUE(result == IdSetError::kInvalidToken);
    MINITEST_EXPECT_STREQ(issue.token, "x");
  }
  {
    Capture capture;
    IdSetIssue issue {};
    std::uint16_t applied = 0;
    MINITEST_EXPECT_TRUE(luce::parse::parse_id_set("", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); }, &issue) == IdSetError::kEmpty);
    MINITEST_EXPECT_TRUE(luce::parse::parse_id_set(" ", 0, 7, false, &applied, [&](std::uint32_t id) { capture(id); }, &issue) ==
                         IdSetError::kInvalidToken);
  }
  {
    Capture capture;
    IdSetIssue issue {};
    std::uint16_t applied = 0;
    const auto result = luce::parse::parse_id_set("1234567890123456789012345678901234567890", 0, 7, false,
                                                  &applied, [&](std::uint32_t id) { capture(id); }, &issue);
    MINITEST_EXPECT_TRUE(result == IdSetError::kInvalidToken);
    MINITEST_EXPECT_EQ(std::strlen(issue.token), static_cast<std::size_t>(31));
  }
}
