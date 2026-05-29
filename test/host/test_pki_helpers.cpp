#include "luce/pki_helpers.h"

#include <cstdint>
#include <cstring>

#include "third_party/minitest.h"

void test_pki_helpers() {
  bool ok = false;
  MINITEST_EXPECT_TRUE(luce::pki::role_from_token("https_server", &ok) == luce::pki::Role::kHttpsServer);
  MINITEST_EXPECT_TRUE(ok);
  MINITEST_EXPECT_TRUE(luce::pki::role_from_token("mqtt", &ok) == luce::pki::Role::kMqttClient);
  MINITEST_EXPECT_TRUE(ok);
  MINITEST_EXPECT_TRUE(luce::pki::role_from_token("ota_client", &ok) == luce::pki::Role::kOtaClient);
  MINITEST_EXPECT_TRUE(ok);
  (void)luce::pki::role_from_token("bad", &ok);
  MINITEST_EXPECT_FALSE(ok);

  MINITEST_EXPECT_STREQ(luce::pki::role_name(luce::pki::Role::kHttpsServer), "https_server");
  MINITEST_EXPECT_STREQ(luce::pki::state_name(luce::pki::State::kKeyReady), "key_present");
  MINITEST_EXPECT_STREQ(luce::pki::state_name(luce::pki::State::kActive), "cert_committed");

  const std::uint8_t mac[6] = {0xAA, 0xBB, 0x00, 0x11, 0x22, 0x33};
  char subject[96] = {};
  luce::pki::make_csr_subject(luce::pki::Role::kHttpsServer, mac, subject, sizeof(subject));
  MINITEST_EXPECT_STREQ(subject, "CN=luce-aabb00112233-https_server,O=LUCE");

  std::uint8_t digest[32] = {};
  for (std::size_t i = 0; i < sizeof(digest); ++i) {
    digest[i] = static_cast<std::uint8_t>(i);
  }
  char fp[96] = {};
  luce::pki::format_hex_fingerprint(digest, sizeof(digest), fp, sizeof(fp));
  MINITEST_EXPECT_EQ(std::strlen(fp), static_cast<std::size_t>(95));
  MINITEST_EXPECT_STREQ(fp, "00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:"
                            "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F");

  MINITEST_EXPECT_TRUE(luce::pki::is_valid_transition(luce::pki::State::kEmpty, luce::pki::State::kKeyReady));
  MINITEST_EXPECT_TRUE(luce::pki::is_valid_transition(luce::pki::State::kKeyReady, luce::pki::State::kCsrReady));
  MINITEST_EXPECT_TRUE(luce::pki::is_valid_transition(luce::pki::State::kCsrReady, luce::pki::State::kCertStaged));
  MINITEST_EXPECT_TRUE(luce::pki::is_valid_transition(luce::pki::State::kCertStaged, luce::pki::State::kActive));
  MINITEST_EXPECT_FALSE(luce::pki::is_valid_transition(luce::pki::State::kEmpty, luce::pki::State::kCsrReady));
  MINITEST_EXPECT_FALSE(luce::pki::is_valid_transition(luce::pki::State::kEmpty, luce::pki::State::kActive));
}
