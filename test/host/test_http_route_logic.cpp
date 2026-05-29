#include "luce/http_route_logic.h"

#include "third_party/minitest.h"

void test_http_route_logic() {
  constexpr int kGet = 0;
  constexpr int kPost = 3;
  constexpr int kPut = 4;
  constexpr std::uint16_t kGetMask = static_cast<std::uint16_t>(1u << kGet);
  constexpr std::uint16_t kPostPutMask = static_cast<std::uint16_t>((1u << kPost) | (1u << kPut));

  MINITEST_EXPECT_TRUE(luce::http::route_method_allowed(kGetMask, kGet));
  MINITEST_EXPECT_FALSE(luce::http::route_method_allowed(kGetMask, kPost));
  MINITEST_EXPECT_FALSE(luce::http::route_method_allowed(kGetMask, 16));

  MINITEST_EXPECT_TRUE(luce::http::route_dispatch_decision(kGetMask, kPost, true, false) ==
                       luce::http::DispatchDecision::kMethodNotAllowed);
  MINITEST_EXPECT_TRUE(luce::http::route_dispatch_decision(kGetMask, kGet, true, false) ==
                       luce::http::DispatchDecision::kUnauthorized);
  MINITEST_EXPECT_TRUE(luce::http::route_dispatch_decision(kGetMask, kGet, true, true) ==
                       luce::http::DispatchDecision::kInvoke);
  MINITEST_EXPECT_TRUE(luce::http::route_dispatch_decision(kPostPutMask, kPut, false, false) ==
                       luce::http::DispatchDecision::kInvoke);
}
