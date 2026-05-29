#include "third_party/minitest.h"

void test_backoff();
void test_http_route_logic();
void test_id_set_parser();
void test_json_writer();
void test_led_manual_mode();
void test_nvs_helpers();
void test_pki_helpers();
void test_relay_logic();
void test_str_utils();

int main() {
  test_id_set_parser();
  test_relay_logic();
  test_backoff();
  test_json_writer();
  test_str_utils();
  test_nvs_helpers();
  test_pki_helpers();
  test_http_route_logic();
  test_led_manual_mode();
  return minitest::finish();
}
