#include "luce/pki.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"

extern "C" {
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/pk_private.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
}

namespace luce {
namespace pki {
namespace {

constexpr const char* kTag = "[PKI]";
constexpr const char* kPkiNs = "pki";
constexpr const char* kKeyAlgEcP256 = "ec-p256";
constexpr std::size_t kRoleCount = 3;
constexpr std::size_t kKeyPemBufferSize = 768;
constexpr std::size_t kCertPemBufferSize = 4096;
constexpr std::size_t kCsrPemBufferSize = 1536;
constexpr std::size_t kSubjectBufferSize = 128;
constexpr std::size_t kFingerprintBufferSize = 96;

struct RoleMaterial {
  char key_pem[kKeyPemBufferSize] = {};
  char cert_pem[kCertPemBufferSize] = {};
  char staged_pem[kCertPemBufferSize] = {};
  char subject[kSubjectBufferSize] = {};
  char issuer[kSubjectBufferSize] = {};
  char fingerprint[kFingerprintBufferSize] = {};
  const char* last_error = "none";
  State state = State::kEmpty;
  bool loaded = false;
  bool read_error = false;
  bool oversized = false;
  bool staged_present = false;
  bool csr_exported = false;
};

RoleMaterial g_roles[kRoleCount]{};

std::size_t role_index(Role role) {
  switch (role) {
  case Role::kHttpsServer:
    return 0;
  case Role::kMqttClient:
    return 1;
  case Role::kOtaClient:
    return 2;
  }
  return 0;
}

const char* role_prefix(Role role) { return role_name(role); }

void key_name(Role role, const char* suffix, char* out, std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return;
  }
  std::snprintf(out, out_size, "%s_%s", role_prefix(role), suffix);
}

RoleMaterial& material(Role role) { return g_roles[role_index(role)]; }

void set_role_state(Role role, State next, const char* reason = nullptr) {
  auto& m = material(role);
  luce::runtime::set_state(m.state, next, state_name, "[PKI]", reason);
}

void set_error(Role role, const char* reason) {
  auto& m = material(role);
  m.last_error = (reason != nullptr && reason[0] != '\0') ? reason : "error";
  set_role_state(role, State::kError, m.last_error);
}

esp_err_t nvs_write_str(const char* key, const char* value) {
  auto nvs = luce::nvs::Handle::Open(kPkiNs, NVS_READWRITE);
  if (!nvs.ok()) {
    return nvs.error();
  }
  ESP_RETURN_ON_ERROR(luce::nvs::write_string(nvs.raw(), key, value), kTag, "nvs_set_str(%s)", key);
  ESP_RETURN_ON_ERROR(luce::nvs::commit(nvs.raw()), kTag, "nvs_commit(%s)", key);
  return ESP_OK;
}

esp_err_t nvs_write_u8_value(const char* key, std::uint8_t value) {
  auto nvs = luce::nvs::Handle::Open(kPkiNs, NVS_READWRITE);
  if (!nvs.ok()) {
    return nvs.error();
  }
  ESP_RETURN_ON_ERROR(nvs_set_u8(nvs.raw(), key, value), kTag, "nvs_set_u8(%s)", key);
  ESP_RETURN_ON_ERROR(luce::nvs::commit(nvs.raw()), kTag, "nvs_commit(%s)", key);
  return ESP_OK;
}

esp_err_t nvs_erase(const char* key) {
  auto nvs = luce::nvs::Handle::Open(kPkiNs, NVS_READWRITE);
  if (!nvs.ok()) {
    return nvs.error();
  }
  esp_err_t err = nvs_erase_key(nvs.raw(), key);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, kTag, "nvs_erase_key(%s)", key);
  ESP_RETURN_ON_ERROR(luce::nvs::commit(nvs.raw()), kTag, "nvs_commit erase %s", key);
  return ESP_OK;
}

int hardware_rng(void*, unsigned char* output, std::size_t output_len) {
  if (output_len == 0) {
    return 0;
  }
  if (output == nullptr) {
    return -1;
  }
  esp_fill_random(output, output_len);
  return 0;
}

void format_subject(Role role, char* out, std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return;
  }
  std::uint8_t mac[6] = {};
  make_csr_subject(role, esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK ? mac : nullptr, out,
                   out_size);
}

void format_sha256_fingerprint(const unsigned char* data, std::size_t len, char* out,
                               std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (data == nullptr || len == 0) {
    return;
  }
  unsigned char digest[32] = {};
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (sha256 == nullptr || mbedtls_md(sha256, data, len, digest) != 0) {
    return;
  }
  format_hex_fingerprint(digest, sizeof(digest), out, out_size);
}

bool validate_pair(Role role, const char* key_pem, const char* cert_pem, char* subject,
                   std::size_t subject_size, char* issuer, std::size_t issuer_size,
                   char* fingerprint, std::size_t fingerprint_size) {
  if (key_pem == nullptr || key_pem[0] == '\0' || cert_pem == nullptr || cert_pem[0] == '\0') {
    set_error(role, "material_missing");
    return false;
  }

  mbedtls_x509_crt crt;
  mbedtls_pk_context key;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&key);

  bool ok = false;
  int rc = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(cert_pem),
                                  std::strlen(cert_pem) + 1);
  if (rc != 0) {
    set_error(role, "cert_parse_failed");
    goto done;
  }
  rc = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(key_pem),
                            std::strlen(key_pem) + 1, nullptr, 0);
  if (rc != 0) {
    set_error(role, "key_parse_failed");
    goto done;
  }
  rc = mbedtls_pk_check_pair(&crt.pk, &key);
  if (rc != 0) {
    set_error(role, "cert_key_mismatch");
    goto done;
  }
  if (subject != nullptr && subject_size > 0) {
    subject[0] = '\0';
    (void)mbedtls_x509_dn_gets(subject, subject_size, &crt.subject);
  }
  if (issuer != nullptr && issuer_size > 0) {
    issuer[0] = '\0';
    (void)mbedtls_x509_dn_gets(issuer, issuer_size, &crt.issuer);
  }
  if (fingerprint != nullptr && fingerprint_size > 0) {
    format_sha256_fingerprint(crt.raw.p, crt.raw.len, fingerprint, fingerprint_size);
  }
  ok = true;

done:
  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&crt);
  return ok;
}

void derive_state(Role role) {
  auto& m = material(role);
  if (m.read_error || m.oversized) {
    set_error(role, m.oversized ? "material_oversized" : "material_read_error");
    return;
  }
  if (m.key_pem[0] == '\0') {
    m.last_error = "none";
    set_role_state(role, State::kEmpty, "empty");
    return;
  }
  if (m.cert_pem[0] == '\0') {
    m.last_error = "none";
    if (m.staged_present) {
      set_role_state(role, State::kCertStaged, "cert_staged");
    } else {
      set_role_state(role, m.csr_exported ? State::kCsrReady : State::kKeyReady,
                     m.csr_exported ? "csr_ready" : "key_present");
    }
    return;
  }
  if (validate_pair(role, m.key_pem, m.cert_pem, m.subject, sizeof(m.subject), m.issuer,
                    sizeof(m.issuer), m.fingerprint, sizeof(m.fingerprint))) {
    m.last_error = "none";
    set_role_state(role, State::kActive, "active");
  }
}

esp_err_t load_role(Role role) {
  auto& m = material(role);
  std::memset(m.key_pem, 0, sizeof(m.key_pem));
  std::memset(m.cert_pem, 0, sizeof(m.cert_pem));
  std::memset(m.staged_pem, 0, sizeof(m.staged_pem));
  std::memset(m.subject, 0, sizeof(m.subject));
  std::memset(m.issuer, 0, sizeof(m.issuer));
  std::memset(m.fingerprint, 0, sizeof(m.fingerprint));
  m.read_error = false;
  m.oversized = false;
  m.staged_present = false;
  m.csr_exported = false;
  m.loaded = true;
  m.last_error = "none";

  auto nvs = luce::nvs::Handle::Open(kPkiNs, NVS_READONLY);
  if (!nvs.ok()) {
    derive_state(role);
    return nvs.error();
  }

  char key[48] = {};
  char cert[48] = {};
  char staged_key[56] = {};
  char csr_key[56] = {};
  key_name(role, "key_pem", key, sizeof(key));
  key_name(role, "cert_pem", cert, sizeof(cert));
  key_name(role, "cert_staged", staged_key, sizeof(staged_key));
  key_name(role, "csr_exported", csr_key, sizeof(csr_key));

  const auto key_status =
      luce::nvs::read_string_status(nvs.raw(), key, m.key_pem, sizeof(m.key_pem), "");
  const auto cert_status =
      luce::nvs::read_string_status(nvs.raw(), cert, m.cert_pem, sizeof(m.cert_pem), "");
  const auto staged_status =
      luce::nvs::read_string_status(nvs.raw(), staged_key, m.staged_pem, sizeof(m.staged_pem), "");
  std::uint8_t csr_exported = 0;
  (void)nvs_get_u8(nvs.raw(), csr_key, &csr_exported);
  m.staged_present = (staged_status == luce::nvs::StringReadStatus::kOk && m.staged_pem[0] != '\0');
  m.csr_exported = (csr_exported != 0);
  m.oversized = (key_status == luce::nvs::StringReadStatus::kOversized ||
                 cert_status == luce::nvs::StringReadStatus::kOversized ||
                 staged_status == luce::nvs::StringReadStatus::kOversized);
  m.read_error = (key_status == luce::nvs::StringReadStatus::kReadError ||
                  cert_status == luce::nvs::StringReadStatus::kReadError ||
                  staged_status == luce::nvs::StringReadStatus::kReadError);
  derive_state(role);
  return ESP_OK;
}

void ensure_loaded(Role role) {
  if (!material(role).loaded) {
    (void)load_role(role);
  }
}

} // namespace

Status get_status(Role role) {
  (void)load_role(role);
  const auto& m = material(role);
  Status status{};
  status.state = m.state;
  status.role = role_name(role);
  status.key_alg = kKeyAlgEcP256;
  status.last_error = m.last_error;
  status.key_present = (m.key_pem[0] != '\0');
  status.cert_present = (m.cert_pem[0] != '\0');
  status.staged_present = m.staged_present;
  status.read_error = m.read_error;
  status.oversized = m.oversized;
  status.key_pem_bytes = std::strlen(m.key_pem);
  status.cert_pem_bytes = std::strlen(m.cert_pem);
  std::snprintf(status.fingerprint, sizeof(status.fingerprint), "%s", m.fingerprint);
  std::snprintf(status.subject, sizeof(status.subject), "%s", m.subject);
  std::snprintf(status.issuer, sizeof(status.issuer), "%s", m.issuer);
  return status;
}

esp_err_t refresh(Role role) { return load_role(role); }

esp_err_t ensure_key(Role role) {
  (void)load_role(role);
  auto& m = material(role);
  if (m.key_pem[0] != '\0') {
    return ESP_OK;
  }

  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_pk_context key;
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_pk_init(&key);

  char personalization[48] = {};
  std::snprintf(personalization, sizeof(personalization), "luce-pki-%s", role_name(role));

  char key_pem_name[48] = {};
  char key_alg_name[48] = {};
  char cert_name[48] = {};
  char csr_key[56] = {};
  key_name(role, "key_pem", key_pem_name, sizeof(key_pem_name));
  key_name(role, "key_alg", key_alg_name, sizeof(key_alg_name));
  key_name(role, "cert_pem", cert_name, sizeof(cert_name));
  key_name(role, "csr_exported", csr_key, sizeof(csr_key));

  int rc = mbedtls_ctr_drbg_seed(&ctr_drbg, hardware_rng, nullptr,
                                 reinterpret_cast<const unsigned char*>(personalization),
                                 std::strlen(personalization));
  if (rc != 0) {
    set_error(role, "keygen_rng_failed");
    goto done;
  }
  rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (rc != 0) {
    set_error(role, "keygen_pk_setup_failed");
    goto done;
  }
  rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random,
                           &ctr_drbg);
  if (rc != 0) {
    set_error(role, "keygen_ec_failed");
    goto done;
  }
  std::memset(m.key_pem, 0, sizeof(m.key_pem));
  rc = mbedtls_pk_write_key_pem(&key, reinterpret_cast<unsigned char*>(m.key_pem),
                                sizeof(m.key_pem));
  if (rc != 0) {
    set_error(role, "keygen_pem_failed");
    goto done;
  }

  if (nvs_write_str(key_pem_name, m.key_pem) != ESP_OK ||
      nvs_write_str(key_alg_name, kKeyAlgEcP256) != ESP_OK) {
    set_error(role, "keygen_nvs_failed");
    rc = -1;
    goto done;
  }
  (void)nvs_erase(cert_name);
  (void)nvs_erase(csr_key);
  m.cert_pem[0] = '\0';
  m.staged_pem[0] = '\0';
  m.subject[0] = '\0';
  m.issuer[0] = '\0';
  m.fingerprint[0] = '\0';
  m.staged_present = false;
  m.csr_exported = false;
  m.last_error = "none";
  set_role_state(role, State::kKeyReady, "key_generated");
  ESP_LOGI(kTag, "%s keygen generated local %s private key; export CSR next", role_name(role),
           kKeyAlgEcP256);

done:
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  if (rc != 0) {
    ESP_LOGE(kTag, "%s keygen failed rc=-0x%04X", role_name(role), static_cast<unsigned>(-rc));
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t export_csr(Role role, char* out, std::size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  (void)load_role(role);
  auto& m = material(role);
  if (m.key_pem[0] == '\0') {
    set_error(role, "csr_key_missing");
    return ESP_ERR_INVALID_STATE;
  }

  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_pk_context key;
  mbedtls_x509write_csr csr;
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_pk_init(&key);
  mbedtls_x509write_csr_init(&csr);

  int rc = 0;
  char subject[kSubjectBufferSize] = {};
  char personalization[48] = {};
  char csr_key[56] = {};
  key_name(role, "csr_exported", csr_key, sizeof(csr_key));
  std::snprintf(personalization, sizeof(personalization), "luce-pki-csr-%s", role_name(role));
  rc = mbedtls_ctr_drbg_seed(&ctr_drbg, hardware_rng, nullptr,
                             reinterpret_cast<const unsigned char*>(personalization),
                             std::strlen(personalization));
  if (rc != 0) {
    set_error(role, "csr_rng_failed");
    goto done;
  }
  rc = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(m.key_pem),
                            std::strlen(m.key_pem) + 1, nullptr, 0);
  if (rc != 0) {
    set_error(role, "csr_key_parse_failed");
    goto done;
  }
  mbedtls_x509write_csr_set_key(&csr, &key);
  mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
  format_subject(role, subject, sizeof(subject));
  rc = mbedtls_x509write_csr_set_subject_name(&csr, subject);
  if (rc != 0) {
    set_error(role, "csr_subject_failed");
    goto done;
  }
  rc = mbedtls_x509write_csr_set_key_usage(&csr, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
  if (rc != 0) {
    set_error(role, "csr_key_usage_failed");
    goto done;
  }
  rc = mbedtls_x509write_csr_pem(&csr, reinterpret_cast<unsigned char*>(out), out_size);
  if (rc != 0) {
    set_error(role, "csr_write_failed");
    goto done;
  }
  if (nvs_write_u8_value(csr_key, 1) != ESP_OK) {
    set_error(role, "csr_marker_failed");
    rc = -1;
    goto done;
  }
  m.csr_exported = true;
  m.last_error = "none";
  set_role_state(role, State::kCsrReady, "csr_exported");

done:
  mbedtls_x509write_csr_free(&csr);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t stage_cert_begin(Role role) {
  char staged_key[56] = {};
  key_name(role, "cert_staged", staged_key, sizeof(staged_key));
  ESP_RETURN_ON_ERROR(nvs_write_str(staged_key, ""), kTag, "stage cert begin");
  (void)load_role(role);
  auto& m = material(role);
  m.staged_pem[0] = '\0';
  m.staged_present = false;
  set_role_state(role, material(role).key_pem[0] != '\0' ? State::kCertStaged : State::kEmpty,
                 "stage_begin");
  return ESP_OK;
}

esp_err_t stage_cert_append(Role role, const char* line) {
  if (line == nullptr || line[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  ensure_loaded(role);
  auto& m = material(role);
  const std::size_t current = std::strlen(m.staged_pem);
  const std::size_t add = std::strlen(line);
  if (current + add + 2 >= sizeof(m.staged_pem)) {
    set_error(role, "cert_staged_too_large");
    return ESP_ERR_NO_MEM;
  }
  std::memcpy(m.staged_pem + current, line, add);
  m.staged_pem[current + add] = '\n';
  m.staged_pem[current + add + 1] = '\0';
  m.staged_present = true;
  set_role_state(role, State::kCertStaged, "cert_append");
  ESP_LOGI(kTag, "%s cert.append ok bytes=%u", role_name(role),
           static_cast<unsigned>(std::strlen(m.staged_pem)));
  return ESP_OK;
}

esp_err_t stage_cert_commit(Role role) {
  ensure_loaded(role);
  auto& m = material(role);
  if (m.key_pem[0] == '\0') {
    set_error(role, "commit_key_missing");
    return ESP_ERR_INVALID_STATE;
  }

  char staged_key[56] = {};
  char cert_key[48] = {};
  key_name(role, "cert_staged", staged_key, sizeof(staged_key));
  key_name(role, "cert_pem", cert_key, sizeof(cert_key));
  if (m.staged_pem[0] == '\0') {
    set_error(role, "commit_staged_missing");
    return ESP_ERR_NOT_FOUND;
  }
  char subject[kSubjectBufferSize] = {};
  char issuer[kSubjectBufferSize] = {};
  char fingerprint[kFingerprintBufferSize] = {};
  if (!validate_pair(role, m.key_pem, m.staged_pem, subject, sizeof(subject), issuer,
                     sizeof(issuer), fingerprint, sizeof(fingerprint))) {
    return ESP_ERR_INVALID_ARG;
  }
  ESP_RETURN_ON_ERROR(nvs_write_str(staged_key, m.staged_pem), kTag, "flush staged cert");
  ESP_RETURN_ON_ERROR(nvs_write_str(cert_key, m.staged_pem), kTag, "promote cert");
  (void)nvs_erase(staged_key);
  std::snprintf(m.cert_pem, sizeof(m.cert_pem), "%s", m.staged_pem);
  m.staged_pem[0] = '\0';
  std::snprintf(m.subject, sizeof(m.subject), "%s", subject);
  std::snprintf(m.issuer, sizeof(m.issuer), "%s", issuer);
  std::snprintf(m.fingerprint, sizeof(m.fingerprint), "%s", fingerprint);
  m.staged_present = false;
  m.last_error = "none";
  set_role_state(role, State::kActive, "cert_committed");
  ESP_LOGI(kTag, "%s cert.commit ok fingerprint=%s subject='%s'", role_name(role), m.fingerprint,
           m.subject);
  return ESP_OK;
}

esp_err_t reset(Role role) {
  char key_pem_name[48] = {};
  char key_alg_name[48] = {};
  char cert_name[48] = {};
  char staged_key[56] = {};
  char csr_key[56] = {};
  key_name(role, "key_pem", key_pem_name, sizeof(key_pem_name));
  key_name(role, "key_alg", key_alg_name, sizeof(key_alg_name));
  key_name(role, "cert_pem", cert_name, sizeof(cert_name));
  key_name(role, "cert_staged", staged_key, sizeof(staged_key));
  key_name(role, "csr_exported", csr_key, sizeof(csr_key));
  (void)nvs_erase(staged_key);
  (void)nvs_erase(cert_name);
  (void)nvs_erase(key_pem_name);
  (void)nvs_erase(key_alg_name);
  (void)nvs_erase(csr_key);
  material(role) = RoleMaterial{};
  material(role).loaded = true;
  set_role_state(role, State::kEmpty, "reset");
  ESP_LOGW(kTag, "%s reset erased local key and certificate", role_name(role));
  return ESP_OK;
}

bool has_key(Role role) {
  ensure_loaded(role);
  return material(role).key_pem[0] != '\0';
}

bool has_cert(Role role) {
  ensure_loaded(role);
  return material(role).cert_pem[0] != '\0';
}

State state(Role role) {
  ensure_loaded(role);
  return material(role).state;
}

const char* private_key_pem_for_tls(Role role) {
  (void)load_role(role);
  return material(role).key_pem;
}

const char* cert_pem_for_tls(Role role) {
  (void)load_role(role);
  return material(role).cert_pem;
}

} // namespace pki
} // namespace luce
