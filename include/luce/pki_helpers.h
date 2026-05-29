#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "luce/str_utils.h"

namespace luce {
namespace pki {

enum class Role : std::uint8_t {
  kHttpsServer = 0,
  kMqttClient,
  kOtaClient,
};

enum class State : std::uint8_t {
  kEmpty = 0,
  kKeyReady,
  kCsrReady,
  kCertStaged,
  kActive,
  kError,
};

inline const char* role_name(Role role) {
  switch (role) {
  case Role::kHttpsServer:
    return "https_server";
  case Role::kMqttClient:
    return "mqtt_client";
  case Role::kOtaClient:
    return "ota_client";
  }
  return "https_server";
}

inline const char* state_name(State state) {
  switch (state) {
  case State::kEmpty:
    return "empty";
  case State::kKeyReady:
    return "key_present";
  case State::kCsrReady:
    return "csr_ready";
  case State::kCertStaged:
    return "cert_staged";
  case State::kActive:
    return "cert_committed";
  case State::kError:
    return "error";
  }
  return "error";
}

inline Role role_from_token(const char* token, bool* ok = nullptr) {
  bool matched = true;
  Role role = Role::kHttpsServer;
  if (luce::str::ascii_iequals(token, "https") || luce::str::ascii_iequals(token, "https_server") ||
      luce::str::ascii_iequals(token, "http")) {
    role = Role::kHttpsServer;
  } else if (luce::str::ascii_iequals(token, "mqtt") ||
             luce::str::ascii_iequals(token, "mqtt_client")) {
    role = Role::kMqttClient;
  } else if (luce::str::ascii_iequals(token, "ota") ||
             luce::str::ascii_iequals(token, "ota_client")) {
    role = Role::kOtaClient;
  } else {
    matched = false;
  }
  if (ok != nullptr) {
    *ok = matched;
  }
  return role;
}

inline bool is_valid_transition(State from, State to) {
  if (to == State::kError || from == to) {
    return true;
  }
  switch (from) {
  case State::kEmpty:
    return to == State::kCsrReady || to == State::kKeyReady;
  case State::kKeyReady:
    return to == State::kCsrReady || to == State::kEmpty;
  case State::kCsrReady:
    return to == State::kCertStaged || to == State::kEmpty;
  case State::kCertStaged:
    return to == State::kActive || to == State::kCsrReady || to == State::kEmpty;
  case State::kActive:
    return to == State::kEmpty || to == State::kCertStaged;
  case State::kError:
    return to == State::kEmpty || to == State::kCsrReady || to == State::kCertStaged ||
           to == State::kActive;
  }
  return false;
}

inline void make_csr_subject(Role role, const std::uint8_t mac[6], char* out,
                             std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return;
  }
  if (mac == nullptr) {
    std::snprintf(out, out_size, "CN=luce-device-%s,O=LUCE", role_name(role));
    return;
  }
  std::snprintf(out, out_size, "CN=luce-%02x%02x%02x%02x%02x%02x-%s,O=LUCE", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5], role_name(role));
}

inline void format_hex_fingerprint(const std::uint8_t* digest, std::size_t digest_len, char* out,
                                   std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (digest == nullptr || digest_len == 0) {
    return;
  }
  std::size_t used = 0;
  for (std::size_t i = 0; i < digest_len && used + 3 < out_size; ++i) {
    const int written =
        std::snprintf(out + used, out_size - used, "%s%02X", i == 0 ? "" : ":", digest[i]);
    if (written <= 0) {
      break;
    }
    used += static_cast<std::size_t>(written);
  }
}

} // namespace pki
} // namespace luce
