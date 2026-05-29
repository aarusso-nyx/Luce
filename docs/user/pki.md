# LUCE On-Device PKI

Date: 2026-05-28

## Scope

LUCE keeps TLS private keys on the device. Operators can create a certificate
signing request, sign it with an external CA, and import the signed certificate,
but there is no CLI, HTTP, MQTT, OTA, or NVS workflow that imports or exports a
private key.

PKI roles are independent:

- `https_server` for the HTTPS API server identity.
- `mqtt_client` for optional MQTT mutual-TLS client identity.
- `ota_client` for optional OTA HTTPS client identity.

Each role uses its own EC P-256 keypair by default. This keeps role compromise
boundaries explicit while sharing one implementation and one NVS namespace.

## State Machine

Each role has a queryable lifecycle:

1. `empty`: no usable role material.
2. `csr_ready`: a device-generated private key exists and a CSR can be exported.
3. `cert_staged`: certificate import is in progress.
4. `cert_committed`: certificate parsed, matches the local key, and is active.
5. `error`: the last transition failed; `pki.status` reports `last_error`.

Transitions fail closed. Certificate commit rejects missing keys, missing staged
material, parse failures, and certificates whose public key does not match the
device-held private key.

## Operator Workflow

Provision a role over the serial CLI:

```text
pki.keygen https_server
pki.csr https_server
pki.cert.begin https_server
pki.cert.append https_server -----BEGIN CERTIFICATE-----
pki.cert.append https_server MIIB...
pki.cert.append https_server -----END CERTIFICATE-----
pki.cert.commit https_server
pki.status https_server
```

Repeat the same flow with `mqtt_client` or `ota_client` when those services need
to present a client certificate.

`pki.status` without a role prints all roles and is allowed over the read-only
TCP CLI. Mutating commands are serial-only.

## HTTPS Compatibility Aliases

The legacy HTTPS CLI names remain as compatibility aliases for the
`https_server` role:

- `tls.keygen` -> `pki.keygen https_server`
- `tls.csr` -> `pki.csr https_server`
- `tls.cert.begin` -> `pki.cert.begin https_server`
- `tls.cert.append <line>` -> `pki.cert.append https_server <line>`
- `tls.cert.commit` -> `pki.cert.commit https_server`
- `tls.reset` -> `pki.reset https_server`
- `tls.status` reports the HTTPS role plus HTTP server startup state

## Storage

PKI material is stored in NVS namespace `pki` with per-role keys:

- `<role>_key_alg`
- `<role>_key_pem`
- `<role>_cert_pem`
- `<role>_cert_staged`

The private-key value is used only by internal TLS consumers and is never printed
in status output. Status surfaces can report presence, PEM byte counts, role
state, SHA-256 certificate fingerprint, subject, issuer, and error reason.

## Consumers

- HTTPS starts only when `https_server` is active.
- MQTT still requires configured broker CA verification for TLS. If
  `mqtt_client` is active, MQTT also presents that client certificate and key.
  If the role is absent or incomplete, client identity stays dormant.
- OTA still requires configured endpoint CA verification for HTTPS. If
  `ota_client` is active, OTA also presents that client certificate and key.
  If the role is absent or incomplete, client identity stays dormant.

Optional client identity can be dormant without blocking MQTT or OTA, but CA
verification remains fail-closed for both services.

## Recovery

Use `pki.reset <role>` only for intentional reprovisioning. It erases the role's
key, certificate, staged certificate, and key algorithm marker. A new keygen and
CSR/sign/import cycle is required afterward.

