#include "cert_store.h"

#include <LittleFS.h>

#include "../badge_log.h"
#include "../config.h"

namespace cert_store {
namespace {

String pathOf(const String &name) { return String(CERTS_DIR) + "/" + name; }

}  // namespace

bool isValidName(const String &name) {
  if (name.length() == 0 || name.length() > 40) return false;
  if (name[0] == '.') return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) return false;
  }
  return true;
}

bool begin() {
  if (!LittleFS.exists(CERTS_DIR)) LittleFS.mkdir(CERTS_DIR);
  badge_log::tagf("cert", "%u certificate(s) installed", (unsigned)count());
  return true;
}

size_t count() {
  File dir = LittleFS.open(CERTS_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  size_t found = 0;
  File entry = dir.openNextFile();
  while (entry && found < MAX_CERTS) {
    if (!entry.isDirectory()) ++found;
    entry = dir.openNextFile();
  }
  dir.close();
  return found;
}

bool nameAt(size_t index, String &out) {
  File dir = LittleFS.open(CERTS_DIR);
  if (!dir || !dir.isDirectory()) return false;

  size_t at = 0;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      if (at == index) {
        out = String(entry.name());
        const int slash = out.lastIndexOf('/');
        if (slash >= 0) out = out.substring(slash + 1);
        dir.close();
        return true;
      }
      ++at;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return false;
}

bool exists(const String &name) {
  return isValidName(name) && LittleFS.exists(pathOf(name));
}

size_t sizeOf(const String &name) {
  if (!isValidName(name)) return 0;
  File file = LittleFS.open(pathOf(name), "r");
  if (!file) return 0;
  const size_t size = file.size();
  file.close();
  return size;
}

String read(const String &name) {
  if (!isValidName(name)) return String();
  File file = LittleFS.open(pathOf(name), "r");
  if (!file) return String();
  if (file.size() > MAX_CERT_BYTES) {
    file.close();
    badge_log::tagf("cert", "'%s' is larger than the %u byte limit", name.c_str(),
                    (unsigned)MAX_CERT_BYTES);
    return String();
  }
  const String pem = file.readString();
  file.close();
  return pem;
}

bool write(const String &name, const uint8_t *data, size_t length) {
  if (!isValidName(name)) return false;
  if (length == 0 || length > MAX_CERT_BYTES) return false;

  // Cap the number of certificates, not just the enumeration. Overwriting an
  // existing name is always allowed; only a genuinely new file is subject to the
  // limit, so an authenticated client cannot fill LittleFS with certs. count()
  // stops enumerating at MAX_CERTS, so "== MAX_CERTS" is really ">=".
  if (!LittleFS.exists(pathOf(name)) && count() >= MAX_CERTS) {
    badge_log::tagf("cert", "refusing '%s': already at the %u certificate limit", name.c_str(),
                    (unsigned)MAX_CERTS);
    return false;
  }

  // A DER file uploaded by mistake would otherwise get as far as the TLS
  // handshake and fail there with nothing useful in the log.
  const String head((const char *)data, length < 64 ? length : 64);
  if (head.indexOf("-----BEGIN CERTIFICATE-----") < 0) {
    badge_log::tagf("cert", "'%s' is not PEM - convert it with `openssl x509 -inform der`",
                    name.c_str());
    return false;
  }

  if (!LittleFS.exists(CERTS_DIR)) LittleFS.mkdir(CERTS_DIR);

  File file = LittleFS.open(pathOf(name), "w");
  if (!file) return false;
  const size_t written = file.write(data, length);
  file.close();

  if (written == length) {
    badge_log::tagf("cert", "stored '%s' (%u bytes)", name.c_str(), (unsigned)length);
    return true;
  }
  return false;
}

bool remove(const String &name) {
  if (!isValidName(name)) return false;
  return LittleFS.remove(pathOf(name));
}

}  // namespace cert_store
