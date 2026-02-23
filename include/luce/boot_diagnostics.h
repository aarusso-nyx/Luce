#pragma once

#include <cstddef>
#include <cstdint>

#include "luce_build.h"
#include "esp_system.h"

const char* luce_reset_reason_to_string(esp_reset_reason_t reason);
std::size_t luce_init_path_reset_reason_line(char* out, std::size_t out_size,
                                            esp_reset_reason_t reason);

void luce_log_heap_integrity(const char* context);
void luce_log_startup_banner();
void luce_print_chip_info();
void luce_print_app_info();
void luce_print_partition_summary();
void luce_print_heap_stats();
