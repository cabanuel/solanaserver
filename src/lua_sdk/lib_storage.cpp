/*
  badge.storage - files and a key/value store.

  File paths are relative to the running app's own directory and cannot leave
  it: no leading slash, no "..". That is the whole sandbox - an app can do what
  it likes inside /apps/<its own id>/ and nothing outside.

  badge.storage.kv is NVS-backed and survives a reflash of the filesystem, which
  makes it the right place for small settings. Keys are capped at 15 characters
  by NVS itself.
*/
#include <Arduino.h>
#include <LittleFS.h>

#include "../apps/app_store.h"
#include "../settings.h"
#include "lua_bindings.h"
#include "lua_runtime.h"

extern "C" {
#include "../lua/lauxlib.h"
#include "../lua/lua.h"
}

namespace bindings {
namespace {

// Resolves and raises on anything that would escape the app directory, so no
// caller has to remember to check.
String checkPath(lua_State *L, int index) {
  const char *relative = luaL_checkstring(L, index);
  const String path = runtime::resolveAppPath(String(relative));
  if (path.length() == 0) {
    luaL_error(L, "storage: '%s' is outside the app directory", relative);
  }
  return path;
}

// Largest file storage.read() will return in one call. Bounds the allocation
// so a big file cannot blow the Lua heap cap or drain internal RAM the radios
// need. Local constant on purpose - config.h is owned by another agent.
static const size_t STORAGE_READ_MAX = 64 * 1024;

int l_read(lua_State *L) {
  const String path = checkPath(L, 1);
  File file = LittleFS.open(path, "r");
  if (!file) {
    lua_pushnil(L);
    lua_pushstring(L, "not found");
    return 2;
  }
  if ((size_t)file.size() > STORAGE_READ_MAX) {
    file.close();
    lua_pushnil(L);
    lua_pushstring(L, "file too large (use storage.size and read in chunks)");
    return 2;
  }
  const String contents = file.readString();
  file.close();
  lua_pushlstring(L, contents.c_str(), contents.length());
  return 1;
}

int writeFile(lua_State *L, bool append) {
  const String path = checkPath(L, 1);
  size_t length = 0;
  const char *data = luaL_checklstring(L, 2, &length);

  // Create any intermediate directories the app asked for.
  const int slash = path.lastIndexOf('/');
  if (slash > 0) {
    const String parent = path.substring(0, slash);
    if (!LittleFS.exists(parent)) LittleFS.mkdir(parent);
  }

  File file = LittleFS.open(path, append ? "a" : "w");
  if (!file) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "cannot open for writing");
    return 2;
  }
  const size_t written = length ? file.write((const uint8_t *)data, length) : 0;
  file.close();

  lua_pushboolean(L, written == length);
  return 1;
}

int l_write(lua_State *L) { return writeFile(L, false); }
int l_append(lua_State *L) { return writeFile(L, true); }

int l_exists(lua_State *L) {
  lua_pushboolean(L, LittleFS.exists(checkPath(L, 1)));
  return 1;
}

int l_size(lua_State *L) {
  File file = LittleFS.open(checkPath(L, 1), "r");
  if (!file) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)file.size());
  file.close();
  return 1;
}

int l_remove(lua_State *L) {
  lua_pushboolean(L, LittleFS.remove(checkPath(L, 1)));
  return 1;
}

int l_mkdir(lua_State *L) {
  lua_pushboolean(L, LittleFS.mkdir(checkPath(L, 1)));
  return 1;
}

// storage.list([subdir]) -> array of names; directories get a trailing slash.
int l_list(lua_State *L) {
  String path;
  if (lua_isnoneornil(L, 1)) {
    const String appId = runtime::currentApp();
    if (appId.length() == 0) return luaL_error(L, "storage: no app running");
    path = app_store::directory(appId);
  } else {
    path = checkPath(L, 1);
  }

  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) {
    lua_pushnil(L);
    lua_pushstring(L, "not a directory");
    return 2;
  }

  // Collected and sorted rather than emitted in directory order. LittleFS
  // returns entries in whatever order they happen to sit in, so an app that
  // pages through files would otherwise reorder itself after every write - and
  // "returns an array of names" reads like it has a defined order.
  constexpr int MAX_ENTRIES = 64;
  String names[MAX_ENTRIES];
  int count = 0;

  File entry = dir.openNextFile();
  while (entry && count < MAX_ENTRIES) {
    String name = String(entry.name());
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    names[count++] = entry.isDirectory() ? name + "/" : name;
    entry = dir.openNextFile();
  }
  dir.close();

  for (int i = 1; i < count; ++i) {
    String key = names[i];
    int j = i - 1;
    while (j >= 0 && names[j] > key) {
      names[j + 1] = names[j];
      --j;
    }
    names[j + 1] = key;
  }

  lua_createtable(L, count, 0);
  for (int i = 0; i < count; ++i) {
    lua_pushstring(L, names[i].c_str());
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

// storage.space() -> used, total (bytes, whole filesystem)
int l_space(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)app_store::usedBytes());
  lua_pushinteger(L, (lua_Integer)app_store::totalBytes());
  return 2;
}

const luaL_Reg FUNCTIONS[] = {
    {"read", l_read},     {"write", l_write},   {"append", l_append},
    {"exists", l_exists}, {"size", l_size},     {"remove", l_remove},
    {"mkdir", l_mkdir},   {"list", l_list},     {"space", l_space},
    {nullptr, nullptr},
};

// -- kv ----------------------------------------------------------------------
// Namespaced per app so two apps using the key "score" do not collide. NVS caps
// keys at 15 characters, so the prefix has to be short. Rather than truncate the
// app id to its first 6 characters - which made every app sharing a 6-char
// prefix collide - the prefix is a stable FNV-1a hash of the *full* app id,
// rendered as 6 hex chars and joined with a ':'.
String appNamespace(const String &appId) {
  uint32_t hash = 2166136261u;  // FNV-1a offset basis
  for (size_t i = 0; i < appId.length(); ++i) {
    hash ^= (uint8_t)appId[i];
    hash *= 16777619u;  // FNV-1a prime
  }
  char buf[7];
  snprintf(buf, sizeof(buf), "%06x", (unsigned)(hash & 0xFFFFFFu));
  return String(buf);
}

String namespacedKey(lua_State *L, int index) {
  const char *key = luaL_checkstring(L, index);
  const size_t keyLen = strlen(key);

  // Build the prefix and the full key inside a scope so no heap-owning String
  // is live if we have to luaL_error (which longjmps over C++ destructors).
  int maxKey = 0;
  bool tooLong = false;
  String full;
  {
    const String prefix = appNamespace(runtime::currentApp());
    maxKey = 15 - (int)prefix.length() - 1;
    tooLong = (int)keyLen > maxKey;
    if (!tooLong) full = prefix + ":" + key;
  }  // prefix destroyed here

  if (tooLong) {
    // `full` is an empty (heap-free) String here, and `prefix` is gone, so the
    // longjmp cannot leak. `key` points into the Lua stack and stays valid.
    luaL_error(L, "kv: key '%s' is too long (max %d chars for this app)", key, maxKey);
  }
  return full;
}

int l_kv_get(lua_State *L) {
  const String key = namespacedKey(L, 1);

  // A key holding "" and a key that was never set both read back empty, so a
  // sentinel is the only way to tell them apart - and the difference matters,
  // because kv.get("x") returning nil is how an app detects first run.
  static const char SENTINEL[] = "\x01\x02missing";
  const String probe = settings::kvGet(key, SENTINEL);
  if (probe == SENTINEL) {
    if (lua_isnoneornil(L, 2)) {
      lua_pushnil(L);
    } else {
      lua_pushvalue(L, 2);  // caller-supplied default, of any type
    }
    return 1;
  }

  lua_pushlstring(L, probe.c_str(), probe.length());
  return 1;
}

int l_kv_set(lua_State *L) {
  const String key = namespacedKey(L, 1);
  size_t length = 0;
  const char *value = luaL_checklstring(L, 2, &length);
  lua_pushboolean(L, settings::kvSet(key, String(value)));
  return 1;
}

int l_kv_remove(lua_State *L) {
  lua_pushboolean(L, settings::kvRemove(namespacedKey(L, 1)));
  return 1;
}

const luaL_Reg KV_FUNCTIONS[] = {
    {"get", l_kv_get}, {"set", l_kv_set}, {"remove", l_kv_remove}, {nullptr, nullptr},
};

}  // namespace

void openStorage(lua_State *L) {
  setTable(L, "storage", FUNCTIONS, nullptr);

  // Attach kv as a sub-table of the storage table we just installed.
  lua_getfield(L, -1, "storage");
  lua_newtable(L);
  luaL_setfuncs(L, KV_FUNCTIONS, 0);
  lua_setfield(L, -2, "kv");
  lua_pop(L, 1);
}

}  // namespace bindings
