#pragma once

#include "driver/gpio.h"

// Dedicated DHT21/22 (single-wire) sensor helper.
// Returns true on valid checksum-verified sample.
bool dht21_22_read_with_retries(gpio_num_t data_pin, float& temperature_c, float& humidity_percent,
                                int attempts = 3);
