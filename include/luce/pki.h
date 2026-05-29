#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "luce/pki_helpers.h"

namespace luce {
namespace pki {

struct Status {
  State state = State::kEmpty;
  const char* role = "";
  const char* key_alg = "ec-p256";
  const char* last_error = "none";
  bool key_present = false;
  bool cert_present = false;
  bool staged_present = false;
  bool read_error = false;
  bool oversized = false;
  std::size_t key_pem_bytes = 0;
  std::size_t cert_pem_bytes = 0;
  char fingerprint[96] = {};
  char subject[128] = {};
  char issuer[128] = {};
};

Status get_status(Role role);
esp_err_t refresh(Role role);
esp_err_t ensure_key(Role role);
esp_err_t export_csr(Role role, char* out, std::size_t out_size);
esp_err_t stage_cert_begin(Role role);
esp_err_t stage_cert_append(Role role, const char* line);
esp_err_t stage_cert_commit(Role role);
esp_err_t reset(Role role);

bool has_key(Role role);
bool has_cert(Role role);
State state(Role role);

// TLS consumers use these pointers only to configure ESP-IDF TLS structs.
// Callers must never log or expose the private-key value.
const char* private_key_pem_for_tls(Role role);
const char* cert_pem_for_tls(Role role);

}  // namespace pki
}  // namespace luce
