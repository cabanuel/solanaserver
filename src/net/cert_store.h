/*
  CA certificates for WPA2-Enterprise, on LittleFS under /certs/.

  These are not baked into the firmware on purpose. A conference RADIUS
  certificate is reissued every year - DEF CON's is named for the year it
  belongs to - so a compiled-in copy would be stale before anyone used it, and
  the badge would then either fail to associate or, worse, be reflashed with
  validation turned off. Uploading the current one takes a single request.

  PEM only. A DER file has to be converted before upload:

      openssl x509 -inform der -in defcon34-wifi.crt -out defcon34-wifi.pem
*/
#pragma once

#include <Arduino.h>

namespace cert_store {

constexpr size_t MAX_CERT_BYTES = 8 * 1024;
constexpr size_t MAX_CERTS = 8;

bool begin();

size_t count();
bool nameAt(size_t index, String &out);
bool exists(const String &name);
size_t sizeOf(const String &name);

// Loads a PEM into memory. Returns an empty string if absent or unreadable.
// The caller owns the copy; nothing here caches it.
String read(const String &name);

// `data` must be PEM. Rejects anything without a BEGIN CERTIFICATE header,
// because a DER file uploaded by mistake otherwise fails much later with an
// opaque TLS error.
bool write(const String &name, const uint8_t *data, size_t length);

bool remove(const String &name);

// Names are restricted to [a-z0-9._-] so they can never escape /certs or need
// escaping in a URL.
bool isValidName(const String &name);

}  // namespace cert_store
