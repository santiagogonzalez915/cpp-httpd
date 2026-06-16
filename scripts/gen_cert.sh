#!/usr/bin/env bash
# Generate a self-signed TLS certificate for local development / demo.
# Output: tls/cert.pem, tls/key.pem
set -e

OUT="$(dirname "$0")/../tls"
mkdir -p "$OUT"

openssl req -x509 -newkey rsa:2048 \
  -keyout "$OUT/key.pem" -out "$OUT/cert.pem" \
  -sha256 -days 365 -nodes \
  -subj "/C=US/ST=CT/L=New Haven/O=cpp-httpd/CN=localhost" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost"

echo "Generated: $OUT/cert.pem and $OUT/key.pem (valid 365 days)"
echo ""
echo "Add to your config:"
echo "  TLSCertFile tls/cert.pem"
echo "  TLSKeyFile  tls/key.pem"
echo "  TLSPort     4443"
