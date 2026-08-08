/*
  The installed-app catalogue on LittleFS.

  Layout:

    /apps/<id>/app.ini      metadata, key=value, one per line
    /apps/<id>/main.lua     entry point (overridable via `entry` in app.ini)
    /apps/<id>/...          anything else the app wants
    /lib/<name>.lua         shared modules, on every app's package.path

  `id` is the directory name and the handle everything else uses: the launcher,
  the push endpoints, badge.system.launch(). It is restricted to
  [a-z0-9._-] so it can never escape /apps or need escaping in a URL.

  app.ini is key=value rather than JSON because it is the only structured file
  the firmware has to parse, and 30 lines of parser beats a dependency.
*/
#pragma once

#include <Arduino.h>

namespace app_store {

struct Info {
  String id;
  String name;
  String version;
  String author;
  String description;
  String entry;  // relative to the app directory, defaults to main.lua
  size_t sizeBytes;
};

bool begin();
bool mounted();

// Rescans /apps. Called at boot and after any install or delete.
void refresh();

size_t count();
bool at(size_t index, Info &out);
bool byId(const String &id, Info &out);
bool exists(const String &id);

// "/apps/<id>" - no trailing slash.
String directory(const String &id);
// Absolute path of the entry script, as fopen() sees it (with the FS_ROOT
// prefix), for handing to Lua.
String entryPath(const String &id);

bool isValidId(const String &id);

// -- Mutation ----------------------------------------------------------------
// Creates the directory if needed. `relativePath` may contain subdirectories;
// they are created as required. Rejects any path containing "..".
bool writeFile(const String &id, const String &relativePath, const uint8_t *data,
               size_t length, bool append = false);
bool readFile(const String &id, const String &relativePath, String &out);
bool removeFile(const String &id, const String &relativePath);
bool removeApp(const String &id);

// Writes app.ini from an Info. Only non-empty fields are emitted.
bool writeManifest(const Info &info);

// -- Filesystem stats --------------------------------------------------------
size_t totalBytes();
size_t usedBytes();

// Lists a directory relative to an app root; entries are names, directories get
// a trailing '/'.
bool listDirectory(const String &id, const String &relativePath, String out[], size_t maxEntries,
                   size_t &countOut);

}  // namespace app_store
