#include "luce/led_manual_mode.h"

#include "third_party/minitest.h"

void test_led_manual_mode() {
  LedManualMode mode = LedManualMode::kAuto;
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("auto", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kAuto);
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("on", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kOn);
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("0", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kOff);
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("blink", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kBlinkNormal);
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("fast", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kBlinkFast);
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("slow", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kBlinkSlow);
  MINITEST_EXPECT_TRUE(parse_led_manual_mode_token("flash", &mode));
  MINITEST_EXPECT_TRUE(mode == LedManualMode::kFlash);
  MINITEST_EXPECT_FALSE(parse_led_manual_mode_token("pulse", &mode));
  MINITEST_EXPECT_STREQ(led_manual_mode_name(LedManualMode::kBlinkFast), "fast");
}
