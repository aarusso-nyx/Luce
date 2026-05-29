#pragma once

#include <cstdint>
#include <cstring>

#include "luce/str_utils.h"

enum class LedManualMode : std::uint8_t {
  kAuto = 0,
  kOff,
  kOn,
  kBlinkNormal,
  kBlinkFast,
  kBlinkSlow,
  kFlash,
};

inline bool parse_led_manual_mode_token(const char* token, LedManualMode* mode) {
  if (token == nullptr || mode == nullptr || token[0] == '\0') {
    return false;
  }
  if (luce::str::ascii_iequals(token, "auto")) {
    *mode = LedManualMode::kAuto;
    return true;
  }
  if (luce::str::ascii_iequals(token, "blink") || luce::str::ascii_iequals(token, "normal")) {
    *mode = LedManualMode::kBlinkNormal;
    return true;
  }
  if (luce::str::ascii_iequals(token, "fast")) {
    *mode = LedManualMode::kBlinkFast;
    return true;
  }
  if (luce::str::ascii_iequals(token, "slow")) {
    *mode = LedManualMode::kBlinkSlow;
    return true;
  }
  if (luce::str::ascii_iequals(token, "flash")) {
    *mode = LedManualMode::kFlash;
    return true;
  }
  bool on = false;
  if (luce::str::parse_bool_token(token, &on)) {
    *mode = on ? LedManualMode::kOn : LedManualMode::kOff;
    return true;
  }
  return false;
}

inline const char* led_manual_mode_name(LedManualMode mode) {
  switch (mode) {
  case LedManualMode::kAuto:
    return "auto";
  case LedManualMode::kOff:
    return "off";
  case LedManualMode::kOn:
    return "on";
  case LedManualMode::kBlinkNormal:
    return "blink";
  case LedManualMode::kBlinkFast:
    return "fast";
  case LedManualMode::kBlinkSlow:
    return "slow";
  case LedManualMode::kFlash:
    return "flash";
  }
  return "auto";
}
