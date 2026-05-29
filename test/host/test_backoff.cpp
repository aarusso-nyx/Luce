#include "luce/backoff.h"

#include "third_party/minitest.h"

void test_backoff() {
  constexpr std::uint32_t kMin = 1000;
  constexpr std::uint32_t kMax = 30000;
  constexpr std::uint32_t kSeed = 0xC0FFEEu;

  std::uint32_t previous_floor = 0;
  for (std::uint32_t attempt = 0; attempt < 12; ++attempt) {
    std::uint32_t floor = kMin;
    for (std::uint32_t i = 0; i < attempt && floor < kMax; ++i) {
      floor = floor > kMax / 2u ? kMax : floor * 2u;
    }
    const std::uint32_t value = luce::backoff::next_backoff_ms(attempt, kMin, kMax, kSeed);
    MINITEST_EXPECT_TRUE(value >= kMin);
    MINITEST_EXPECT_TRUE(value >= floor);
    MINITEST_EXPECT_TRUE(value <= kMax);
    MINITEST_EXPECT_TRUE(floor >= previous_floor);
    previous_floor = floor;
    MINITEST_EXPECT_EQ(value, luce::backoff::next_backoff_ms(attempt, kMin, kMax, kSeed));
    MINITEST_EXPECT_TRUE(value <= floor + floor / 4u || value == kMax);
  }

  MINITEST_EXPECT_EQ(luce::backoff::next_backoff_ms(0, kMin, kMax, kSeed), 1000u + luce::backoff::bounded_jitter(250, kSeed));
  MINITEST_EXPECT_EQ(luce::backoff::next_backoff_ms(3, 0, kMax, kSeed), 0u);
  MINITEST_EXPECT_EQ(luce::backoff::next_backoff_ms(3, kMin, 0, kSeed), 0u);
  MINITEST_EXPECT_EQ(luce::backoff::next_backoff_ms(0, 5000, 1000, kSeed), 1000u);
}
