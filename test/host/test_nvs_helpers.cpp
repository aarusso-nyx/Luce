#include "luce/nvs_helpers.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "third_party/minitest.h"

namespace {

enum class FakeNvsMode {
  kMissing,
  kOk,
  kOversized,
  kReadError,
};

FakeNvsMode g_mode = FakeNvsMode::kMissing;
const char* g_value = "";

}  // namespace

esp_err_t nvs_get_str(nvs_handle_t, const char*, char* out_value, std::size_t* length) {
  if (g_mode == FakeNvsMode::kMissing) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (g_mode == FakeNvsMode::kReadError) {
    *length = 1;
    return ESP_ERR_INVALID_STATE;
  }
  const std::size_t needed = std::strlen(g_value) + 1;
  if (out_value == nullptr) {
    *length = g_mode == FakeNvsMode::kOversized ? needed + 64 : needed;
    return ESP_OK;
  }
  if (*length < needed) {
    return ESP_ERR_INVALID_STATE;
  }
  std::snprintf(out_value, *length, "%s", g_value);
  return ESP_OK;
}

esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t, const char*, std::uint8_t*) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_u16(nvs_handle_t, const char*, std::uint16_t*) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_u32(nvs_handle_t, const char*, std::uint32_t*) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u8(nvs_handle_t, const char*, std::uint8_t) { return ESP_OK; }
esp_err_t nvs_set_u16(nvs_handle_t, const char*, std::uint16_t) { return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t, const char*, std::uint32_t) { return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_OK; }

void test_nvs_helpers() {
  char out[8] = {};

  g_mode = FakeNvsMode::kMissing;
  MINITEST_EXPECT_TRUE(luce::nvs::read_string_status(1, "k", out, sizeof(out), "fb") ==
                       luce::nvs::StringReadStatus::kMissing);
  MINITEST_EXPECT_STREQ(out, "fb");

  g_mode = FakeNvsMode::kReadError;
  MINITEST_EXPECT_TRUE(luce::nvs::read_string_status(1, "k", out, sizeof(out), "fb") ==
                       luce::nvs::StringReadStatus::kReadError);
  MINITEST_EXPECT_STREQ(out, "fb");

  g_mode = FakeNvsMode::kOversized;
  g_value = "toolong";
  MINITEST_EXPECT_TRUE(luce::nvs::read_string_status(1, "k", out, sizeof(out), "fb") ==
                       luce::nvs::StringReadStatus::kOversized);
  MINITEST_EXPECT_STREQ(out, "fb");

  g_mode = FakeNvsMode::kOk;
  g_value = "ok";
  MINITEST_EXPECT_TRUE(luce::nvs::read_string_status(1, "k", out, sizeof(out), "fb") ==
                       luce::nvs::StringReadStatus::kOk);
  MINITEST_EXPECT_STREQ(out, "ok");
}
