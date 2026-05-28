#!/usr/bin/env bash
#
# Generate a self-signed dev-mode certificate + private key for the Luce
# HTTPS server. The resulting tools/dev_cert.pem and tools/dev_key.pem
# files are picked up at build time by src/CMakeLists.txt and embedded
# into the firmware as fallback TLS material when:
#   - tls_dev_mode is enabled in NVS, AND
#   - http/cert_pem and http/key_pem are empty
#
# This is a developer-shared cert: every unit built from the same
# tools/dev_*.pem pair serves the same cert. That's fine for benches
# and lab fleets; production units MUST provision their own cert+key
# via the serial CLI (see docs/user/http.md "TLS provisioning").
#
# Usage:
#   ./tools/gen_dev_cert.sh              # CN=luce-dev.local, 10y validity
#   ./tools/gen_dev_cert.sh luce-bench.local  # custom CN/SAN
#
# Output files:
#   tools/dev_cert.pem
#   tools/dev_key.pem
# Both are .gitignored — the private key never enters version control.

set -euo pipefail

cd "$(dirname "$0")/.."

CN="${1:-luce-dev.local}"
CERT_OUT="tools/dev_cert.pem"
KEY_OUT="tools/dev_key.pem"
DAYS="${LUCE_DEV_CERT_DAYS:-3650}"

cat > /tmp/luce_dev_cert.cnf <<EOF
[req]
distinguished_name = req_distinguished_name
x509_extensions = v3_req
prompt = no

[req_distinguished_name]
CN = ${CN}

[v3_req]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = ${CN}
DNS.2 = luce-dev
DNS.3 = localhost
IP.1  = 127.0.0.1
EOF

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
    -keyout "${KEY_OUT}" -out "${CERT_OUT}" \
    -days "${DAYS}" -nodes \
    -config /tmp/luce_dev_cert.cnf -extensions v3_req >/dev/null 2>&1

rm -f /tmp/luce_dev_cert.cnf

# Lock the key down so other users on the same host can't read it.
chmod 600 "${KEY_OUT}"
chmod 644 "${CERT_OUT}"

echo "Wrote ${CERT_OUT} and ${KEY_OUT}"
echo "  CN/SAN: ${CN}"
echo "  Validity: ${DAYS} days"
SHA=$(openssl x509 -in "${CERT_OUT}" -fingerprint -sha256 -noout | sed 's/.*=//' | tr -d ':' | tr 'A-F' 'a-f')
echo "  SHA-256: ${SHA}"
echo
echo "Rebuild the firmware to embed; rerun this script any time you want a fresh cert."
