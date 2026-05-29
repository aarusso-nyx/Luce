#include "luce/relay_logic.h"

#include "third_party/minitest.h"

void test_relay_logic() {
  for (unsigned value = 0; value <= 0xFFu; ++value) {
    const auto state = static_cast<std::uint8_t>(value);
    MINITEST_EXPECT_EQ(luce::relay::output_to_state_mask(luce::relay::state_to_output_mask(state, false), false), state);
    MINITEST_EXPECT_EQ(luce::relay::output_to_state_mask(luce::relay::state_to_output_mask(state, true), true), state);
  }

  MINITEST_EXPECT_EQ(luce::relay::apply_night_policy(0xFFu, 0x0Fu, false), 0xFFu);
  MINITEST_EXPECT_EQ(luce::relay::apply_night_policy(0xFFu, 0x0Fu, true), 0xF0u);
  MINITEST_EXPECT_EQ(luce::relay::apply_night_policy(0x55u, 0x0Fu, true), 0x50u);
  MINITEST_EXPECT_EQ(luce::relay::apply_night_policy(0x55u, 0xF0u, true), 0x05u);

  std::uint8_t mask = 0;
  mask = static_cast<std::uint8_t>(mask | (1u << 3));
  MINITEST_EXPECT_EQ(mask, 0x08u);
  mask = static_cast<std::uint8_t>(mask & ~(1u << 3));
  MINITEST_EXPECT_EQ(mask, 0x00u);
}
