#include "app_store.h"

#include <LittleFS.h>

#include "../badge_log.h"
#include "../config.h"

namespace app_store {
namespace {

constexpr size_t MAX_APPS = 32;

Info sApps[MAX_APPS];
size_t sCount = 0;
bool sMounted = false;

// Defined below; declared here so loadManifest() can validate the manifest's
// `entry` field the same way every other network-sourced path is validated.
bool isSafeRelativePath(const String &path);

// Reads app.ini into `out`. Missing file is not an error: an app that is just a
// main.lua still shows up, named after its directory.
void loadManifest(const String &id, Info &out) {
  out.id = id;
  out.name = id;
  out.version = "";
  out.author = "";
  out.description = "";
  out.entry = APP_ENTRY;

  File file = LittleFS.open(directory(id) + "/" + APP_MANIFEST, "r");
  if (!file) return;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;

    const int separator = line.indexOf('=');
    if (separator <= 0) continue;

    String key = line.substring(0, separator);
    String value = line.substring(separator + 1);
    key.trim();
    key.toLowerCase();
    value.trim();

    if (key == "name") out.name = value;
    else if (key == "version") out.version = value;
    else if (key == "author") out.author = value;
    else if (key == "description") out.description = value;
    else if (key == "entry" && value.length()) {
      // The manifest can arrive in a pushed app, so `entry` is as untrusted as
      // any other path off the network. Every other path goes through
      // isSafeRelativePath(); this one used to be taken verbatim, which let an
      // entry of "../../.." escape the app directory when entryPath() built the
      // absolute path handed to Lua. Reject traversal/absolute/bad charset and
      // keep the APP_ENTRY default set above.
      if (isSafeRelativePath(value)) {
        out.entry = value;
      } else {
        badge_log::tagf("app", "app '%s': unsafe entry '%s' ignored, using %s", id.c_str(),
                        value.c_str(), APP_ENTRY);
      }
    }
  }
  file.close();
}

size_t directorySize(const String &path) {
  size_t total = 0;
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return 0;
  File entry = dir.openNextFile();
  while (entry) {
    total += entry.isDirectory() ? directorySize(String(entry.path())) : entry.size();
    entry = dir.openNextFile();
  }
  return total;
}

bool removeRecursive(const String &path) {
  File probe = LittleFS.open(path);
  if (!probe) return false;
  if (!probe.isDirectory()) {
    probe.close();
    return LittleFS.remove(path);
  }
  probe.close();

  // Delete in bounded passes. Deleting while the directory iterator is open is
  // not reliable on LittleFS, so each pass snapshots a batch of names, closes
  // the directory, deletes them, then reopens and repeats until the directory
  // is empty. The old code snapshotted into a fixed 48-slot array in one shot
  // and silently ignored the rest, so a directory with more than 48 entries was
  // left partly populated and the final rmdir() failed - the app half-vanished.
  constexpr size_t BATCH = 32;
  bool ok = true;
  for (;;) {
    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      ok = false;
      break;
    }

    String children[BATCH];
    bool isDirectory[BATCH];
    size_t found = 0;
    File entry = dir.openNextFile();
    while (entry && found < BATCH) {
      children[found] = String(entry.path());
      isDirectory[found] = entry.isDirectory();
      ++found;
      entry = dir.openNextFile();
    }
    dir.close();

    if (found == 0) break;  // nothing left in this directory

    size_t removed = 0;
    for (size_t i = 0; i < found; ++i) {
      const bool gone =
          isDirectory[i] ? removeRecursive(children[i]) : LittleFS.remove(children[i]);
      if (gone) {
        ++removed;
      } else {
        ok = false;
      }
    }

    // No progress in a full pass means an entry is genuinely stuck; stop rather
    // than spin forever re-reading the same undeletable names.
    if (removed == 0) {
      ok = false;
      break;
    }
  }

  // Attempt the rmdir regardless so a fully-emptied directory is always removed;
  // report success only when the tree came apart cleanly and the dir is gone.
  const bool dirRemoved = LittleFS.rmdir(path);
  return ok && dirRemoved;
}

// Creates every missing directory along `path` (which must be absolute).
bool ensureDirectory(const String &path) {
  if (path.length() == 0 || path == "/") return true;
  if (LittleFS.exists(path)) return true;

  int slash = path.indexOf('/', 1);
  while (slash > 0) {
    const String parent = path.substring(0, slash);
    if (!LittleFS.exists(parent)) LittleFS.mkdir(parent);
    slash = path.indexOf('/', slash + 1);
  }
  return LittleFS.mkdir(path) || LittleFS.exists(path);
}

// Guards every path that came in over the network: no traversal, no absolute
// paths, no empty segments.
bool isSafeRelativePath(const String &path) {
  if (path.length() == 0 || path.length() > 96) return false;
  if (path.startsWith("/")) return false;
  if (path.indexOf("..") >= 0) return false;
  if (path.indexOf("//") >= 0) return false;
  for (size_t i = 0; i < path.length(); ++i) {
    const char c = path[i];
    const bool ok = isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '/';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool isValidId(const String &id) {
  if (id.length() == 0 || id.length() > 32) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    const char c = id[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) return false;
  }
  // A leading dot would let an id hide from the launcher's listing.
  return id[0] != '.';
}

bool begin() {
  // format-on-fail: a badge that has never been written to has no filesystem,
  // and refusing to boot over that would be unhelpful.
  sMounted = LittleFS.begin(true);
  if (!sMounted) {
    badge_log::tagf("fs", "LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists(APPS_DIR)) LittleFS.mkdir(APPS_DIR);
  if (!LittleFS.exists(LIB_DIR)) LittleFS.mkdir(LIB_DIR);

  badge_log::tagf("fs", "LittleFS mounted, %u/%u KB used", (unsigned)(usedBytes() / 1024),
                  (unsigned)(totalBytes() / 1024));
  refresh();
  return true;
}

bool mounted() { return sMounted; }

void refresh() {
  sCount = 0;
  if (!sMounted) return;

  File dir = LittleFS.open(APPS_DIR);
  if (!dir || !dir.isDirectory()) return;

  File entry = dir.openNextFile();
  while (entry && sCount < MAX_APPS) {
    if (entry.isDirectory()) {
      String id = String(entry.name());
      const int slash = id.lastIndexOf('/');
      if (slash >= 0) id = id.substring(slash + 1);

      if (isValidId(id)) {
        loadManifest(id, sApps[sCount]);
        sApps[sCount].sizeBytes = directorySize(directory(id));
        ++sCount;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();
  badge_log::tagf("fs", "%u app(s) installed", (unsigned)sCount);
}

size_t count() { return sCount; }

bool at(size_t index, Info &out) {
  if (index >= sCount) return false;
  out = sApps[index];
  return true;
}

bool byId(const String &id, Info &out) {
  for (size_t i = 0; i < sCount; ++i) {
    if (sApps[i].id == id) {
      out = sApps[i];
      return true;
    }
  }
  return false;
}

bool exists(const String &id) {
  return isValidId(id) && sMounted && LittleFS.exists(directory(id));
}

String directory(const String &id) { return String(APPS_DIR) + "/" + id; }

String entryPath(const String &id) {
  Info info;
  String entry = byId(id, info) ? info.entry : String(APP_ENTRY);
  // loadManifest() already rejects an unsafe `entry`, but this string is about
  // to become an absolute fopen() path handed to Lua, so re-check rather than
  // trust the cached Info - a belt-and-suspenders guard against traversal.
  if (!isSafeRelativePath(entry)) entry = String(APP_ENTRY);
  return String(FS_ROOT) + directory(id) + "/" + entry;
}

bool writeFile(const String &id, const String &relativePath, const uint8_t *data, size_t length,
               bool append) {
  if (!sMounted || !isValidId(id) || !isSafeRelativePath(relativePath)) return false;

  const String base = directory(id);
  if (!ensureDirectory(base)) return false;

  const String full = base + "/" + relativePath;
  const int slash = full.lastIndexOf('/');
  if (slash > 0 && !ensureDirectory(full.substring(0, slash))) return false;

  File file = LittleFS.open(full, append ? "a" : "w");
  if (!file) return false;
  const size_t written = length ? file.write(data, length) : 0;
  file.close();
  return written == length;
}

bool readFile(const String &id, const String &relativePath, String &out) {
  if (!sMounted || !isValidId(id) || !isSafeRelativePath(relativePath)) return false;
  File file = LittleFS.open(directory(id) + "/" + relativePath, "r");
  if (!file) return false;
  out = file.readString();
  file.close();
  return true;
}

bool removeFile(const String &id, const String &relativePath) {
  if (!sMounted || !isValidId(id) || !isSafeRelativePath(relativePath)) return false;
  return LittleFS.remove(directory(id) + "/" + relativePath);
}

bool removeApp(const String &id) {
  if (!sMounted || !isValidId(id)) return false;
  const bool ok = removeRecursive(directory(id));
  refresh();
  return ok;
}

bool writeManifest(const Info &info) {
  if (!isValidId(info.id)) return false;
  String text;
  text += "name=" + (info.name.length() ? info.name : info.id) + "\n";
  if (info.version.length()) text += "version=" + info.version + "\n";
  if (info.author.length()) text += "author=" + info.author + "\n";
  if (info.description.length()) text += "description=" + info.description + "\n";
  if (info.entry.length() && info.entry != APP_ENTRY) text += "entry=" + info.entry + "\n";
  return writeFile(info.id, APP_MANIFEST, (const uint8_t *)text.c_str(), text.length(), false);
}

size_t totalBytes() { return sMounted ? LittleFS.totalBytes() : 0; }
size_t usedBytes() { return sMounted ? LittleFS.usedBytes() : 0; }

bool listDirectory(const String &id, const String &relativePath, String out[], size_t maxEntries,
                   size_t &countOut) {
  countOut = 0;
  if (!sMounted || !isValidId(id)) return false;
  if (relativePath.length() && !isSafeRelativePath(relativePath)) return false;

  String path = directory(id);
  if (relativePath.length()) path += "/" + relativePath;

  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return false;

  File entry = dir.openNextFile();
  while (entry && countOut < maxEntries) {
    String name = String(entry.name());
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    out[countOut++] = entry.isDirectory() ? name + "/" : name;
    entry = dir.openNextFile();
  }
  dir.close();
  return true;
}

}  // namespace app_store
