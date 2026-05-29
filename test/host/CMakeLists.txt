cmake_minimum_required(VERSION 3.16)

project(luce_host_tests LANGUAGES CXX)

add_executable(luce_host_tests
  test_main.cpp
  test_backoff.cpp
  test_http_route_logic.cpp
  test_id_set_parser.cpp
  test_json_writer.cpp
  test_led_manual_mode.cpp
  test_nvs_helpers.cpp
  test_pki_helpers.cpp
  test_relay_logic.cpp
  test_str_utils.cpp
)
target_compile_features(luce_host_tests PRIVATE cxx_std_17)
target_include_directories(luce_host_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_compile_options(luce_host_tests PRIVATE -Wall -Wextra -Werror)
