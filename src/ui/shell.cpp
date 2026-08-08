#include "shell.h"

#include "../apps/app_store.h"
#include "../badge_log.h"
#include "../config.h"
#include "../hal/badge_i2c.h"
#include "../hal/buttons.h"
#include "../hal/display.h"
#include "../hal/leds.h"
#include "../hal/mic.h"
#include "../hal/power.h"
#include "../hal/se050.h"
#include "../identity/identity.h"
#include "../lua_sdk/lua_runtime.h"
#include "../net/ble_mgr.h"
#include "../net/broker_client.h"
#include "../net/espnow_mgr.h"
#include "../net/push_server.h"
#include "../net/wifi_mgr.h"
#include "../settings.h"
#include "theme.h"

namespace shell {
namespace {

enum class Screen : uint8_t {
  Launcher,
  Settings,
  Wifi,
  Bluetooth,
  Espnow,
  Push,
  Broker,
  Identity,
  IdentityNew,
  Display,
  Leds,
  Info,
  Console,
  AppError,
  // Reached from the launcher with RIGHT on an installed app. Its own screen
  // rather than an inline confirm, because deleting is irreversible and the
  // wearer should see which app they are about to lose.
  AppDelete,
  // Raised automatically by update() when the broker has something waiting;
  // not reachable from any menu.
  Offer,
  OfferInstalling,
};

Screen sScreen = Screen::Launcher;
int sCursor = 0;
int sScroll = 0;
bool sDirty = true;
String sError;

// The app the delete confirmation is about. Held by id rather than by launcher
// index: a push can install or remove an app between the prompt going up and
// SELECT being pressed, and an index would then point at a different app than
// the one on screen.
String sDeleteId;
String sDeleteName;

// Screens that show live values (signal strength, battery, mic level) repaint on
// a timer as well as on input.
uint32_t sLastRepaintAt = 0;
constexpr uint32_t LIVE_REPAINT_MS = 250;

// Set when update() raises the Offer prompt, so updateOffer() can ignore the
// button edges computed on the entry tick. Without this an in-flight SELECT from
// the previous screen would count as consent and install pushed code to flash
// before the wearer ever saw the prompt.
bool sOfferJustRaised = false;

// Millis the OfferInstalling screen began waiting for a result. A stalled
// install (network dropped mid-download, broker never returns a result string)
// would otherwise trap the badge until reset, since the long-hold-B force-quit
// only applies to Lua apps and not to shell screens. After this long, B leaves.
uint32_t sInstallWaitStart = 0;
constexpr uint32_t INSTALL_ESCAPE_MS = 20000;

// The launcher's only live content is the installed-app count (installs and
// deletes arrive over the network) and the battery percent in the status bar.
// Watched here so the launcher repaints when one of them moves rather than on a
// blind 4Hz full-screen repaint. See update().
size_t sLauncherAppCount = 0;
int sLauncherBattery = -1;

// Fixed rows at the top of the Wi-Fi screen, above the scan results. Shared by
// the draw and update halves so the two cannot disagree about where the scan
// list starts.
constexpr int WIFI_ACTIONS = 5;

// Rows on the Settings menu. Named for the same reason as WIFI_ACTIONS: the
// draw half and the update half must never disagree about how many there are,
// and adding a screen previously meant remembering to bump two literals.
constexpr int SETTINGS_ITEMS = 10;

constexpr int ROW_HEIGHT = 22;
constexpr int LIST_TOP = display::STATUS_BAR_HEIGHT + 8;
constexpr int FOOTER_HEIGHT = 18;

inline int listRows() {
  return (display::height() - LIST_TOP - FOOTER_HEIGHT) / ROW_HEIGHT;
}

void repaint() { sDirty = true; }

void go(Screen screen) {
  sScreen = screen;
  sCursor = 0;
  sScroll = 0;
  repaint();
}

// ---------------------------------------------------------------------------
// Shared chrome
// ---------------------------------------------------------------------------

void footer(const char *hint) {
  auto &canvas = display::canvas();
  const int y = display::height() - FOOTER_HEIGHT;
  canvas.fillRect(0, y, display::width(), FOOTER_HEIGHT, theme::HEADER);
  display::text(hint, 8, y + 5, theme::MUTED, 1);
}

// Keeps `sCursor` in range and scrolls the window to follow it.
void clampCursor(int count) {
  if (count <= 0) {
    sCursor = 0;
    sScroll = 0;
    return;
  }
  if (sCursor < 0) sCursor = count - 1;      // wrap up
  if (sCursor >= count) sCursor = 0;         // wrap down
  const int rows = listRows();
  if (sCursor < sScroll) sScroll = sCursor;
  if (sCursor >= sScroll + rows) sScroll = sCursor - rows + 1;
  if (sScroll < 0) sScroll = 0;
}

// Draws one selectable row. `value`, when given, is right-aligned - that is how
// every toggle and slider in Settings renders.
void row(int slot, const char *label, const char *value, bool selected, uint16_t valueColor) {
  auto &canvas = display::canvas();
  const int y = LIST_TOP + slot * ROW_HEIGHT;
  if (selected) {
    canvas.fillRoundRect(4, y - 2, display::width() - 8, ROW_HEIGHT - 2, 4, theme::PANEL_2);
    // A brand-purple bar marks the selection without relying on colour alone
    // being visible at a glance.
    canvas.fillRect(4, y - 2, 3, ROW_HEIGHT - 2, theme::PURPLE);
  }
  display::text(label, 12, y + 3, selected ? theme::WHITE : theme::TEXT, 1);
  if (value && value[0]) {
    display::textRight(value, display::width() - 12, y + 3, valueColor, 1);
  }
}

// A generic menu: returns true if `key` was consumed.
struct MenuItem {
  const char *label;
  const char *value;
  uint16_t valueColor;
};

void drawMenu(const char *title, const MenuItem *items, int count, const char *hint) {
  display::canvas().fillScreen(theme::BG);
  display::statusBar(title);
  clampCursor(count);

  const int rows = listRows();
  for (int i = 0; i < rows && (sScroll + i) < count; ++i) {
    const MenuItem &item = items[sScroll + i];
    row(i, item.label, item.value, (sScroll + i) == sCursor, item.valueColor);
  }

  if (count > rows) {
    char position[16];
    snprintf(position, sizeof(position), "%d/%d", sCursor + 1, count);
    display::textRight(position, display::width() - 8, display::height() - FOOTER_HEIGHT - 14,
                       theme::MUTED, 1);
  }
  footer(hint);
}

// Up/down/wrap for any list. Returns true if the cursor moved.
bool moveCursor(int count) {
  if (count <= 0) return false;
  if (buttons::repeated(BTN_UP)) {
    --sCursor;
    clampCursor(count);
    return true;
  }
  if (buttons::repeated(BTN_DOWN)) {
    ++sCursor;
    clampCursor(count);
    return true;
  }
  return false;
}

// Left/right on a 0..255 value, with a coarse step. Returns the new value.
uint8_t adjust(uint8_t value, int step) {
  int next = (int)value;
  if (buttons::repeated(BTN_LEFT)) next -= step;
  if (buttons::repeated(BTN_RIGHT)) next += step;
  return (uint8_t)constrain(next, 0, 255);
}

const char *onOff(bool value) { return value ? "on" : "off"; }
uint16_t onOffColor(bool value) { return value ? theme::GREEN : theme::MUTED; }

// Five bars from an RSSI. -50 and better is full, -95 and worse is empty.
int signalBars(int rssi) {
  if (rssi >= -50) return 5;
  if (rssi >= -60) return 4;
  if (rssi >= -70) return 3;
  if (rssi >= -80) return 2;
  if (rssi >= -90) return 1;
  return 0;
}

void drawSignalBars(int x, int y, int rssi) {
  auto &canvas = display::canvas();
  const int bars = signalBars(rssi);
  for (int i = 0; i < 5; ++i) {
    const int h = 3 + i * 2;
    const uint16_t color = i < bars ? theme::gradient565((float)i / 4.0f) : theme::BORDER;
    canvas.fillRect(x + i * 4, y + (11 - h), 3, h, color);
  }
}

// ---------------------------------------------------------------------------
// Launcher
// ---------------------------------------------------------------------------

void drawLauncher() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar(SOLANA_OS_NAME);

  const int count = (int)app_store::count() + 1;  // apps, then Settings
  clampCursor(count);
  const int rows = listRows();

  if (app_store::count() == 0 && sScroll == 0) {
    display::textCentered("No apps installed", display::width() / 2, 58, theme::MUTED, 1);
    display::textCentered("Push one over Wi-Fi or BLE", display::width() / 2, 76, theme::MUTED, 1);
    display::textCentered("Settings > Push for the address", display::width() / 2, 94,
                          theme::MUTED, 1);
    display::textCentered("or send one from the App Store", display::width() / 2, 116,
                          theme::MUTED, 1);
    // The badge ID is the whole interaction on the sender's side, so it is
    // worth showing where an empty launcher would otherwise show nothing.
    const String id = identity::badgeId();
    display::textCentered(id.length() ? ("badge ID " + id).c_str() : "Settings > Identity",
                          display::width() / 2, 134, theme::PURPLE, 1);
  }

  app_store::Info info;
  for (int i = 0; i < rows && (sScroll + i) < count; ++i) {
    const int index = sScroll + i;
    const bool selected = index == sCursor;

    if (index == count - 1) {
      row(i, "Settings", ">", selected, theme::MUTED);
      continue;
    }
    if (!app_store::at((size_t)index, info)) continue;

    // Version on the right when there is one; it is the field most likely to
    // matter when several copies of an app have been pushed.
    row(i, info.name.c_str(), info.version.length() ? info.version.c_str() : "", selected,
        theme::MUTED);
  }

  // "right delete" only when the cursor is on an app - offering it on the
  // Settings row would be a lie, and the footer is the only place the shortcut
  // is discoverable at all.
  footer(sCursor < count - 1 ? "SELECT run   right delete   CANCEL settings"
                             : "SELECT run   up/down move   CANCEL settings");
}

void updateLauncher() {
  const int count = (int)app_store::count() + 1;
  if (moveCursor(count)) repaint();

  if (buttons::pressed(BTN_A)) {
    if (sCursor == count - 1) {
      go(Screen::Settings);
      return;
    }
    app_store::Info info;
    if (app_store::at((size_t)sCursor, info)) {
      leds::stopAnimation();
      runtime::requestLaunch(info.id);
    }
  }

  // RIGHT rather than a long press or a Settings submenu: the launcher is where
  // the apps are, and right/left are unbound here anyway. It only arms the
  // confirmation - nothing is removed until SELECT on the next screen.
  if (buttons::pressed(BTN_RIGHT) && sCursor < count - 1) {
    app_store::Info info;
    if (app_store::at((size_t)sCursor, info)) {
      sDeleteId = info.id;
      sDeleteName = info.name.length() ? info.name : info.id;
      go(Screen::AppDelete);
      return;
    }
  }

  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// Settings menu
// ---------------------------------------------------------------------------

void drawSettings() {
  char wifiValue[24];
  snprintf(wifiValue, sizeof(wifiValue), "%s", wifi_mgr::statusText());
  char peers[16];
  snprintf(peers, sizeof(peers), "%u peer%s", (unsigned)espnow_mgr::peerCount(),
           espnow_mgr::peerCount() == 1 ? "" : "s");
  const String badgeId = identity::badgeId();

  const MenuItem items[] = {
      {"Wi-Fi", wifiValue, wifi_mgr::connected() ? theme::GREEN : theme::MUTED},
      {"Bluetooth", onOff(ble_mgr::enabled()), onOffColor(ble_mgr::enabled())},
      {"ESP-NOW", espnow_mgr::enabled() ? peers : "off", onOffColor(espnow_mgr::enabled())},
      {"App push", push_server::running() ? "ready" : "needs wi-fi",
       push_server::running() ? theme::GREEN : theme::MUTED},
      {"App store", broker::stateText(),
       broker::state() == broker::State::Error ? theme::ERR
       : broker::registered()                  ? theme::GREEN
                                               : theme::MUTED},
      // Held in a named local for the same reason as the Wi-Fi screen's
      // savedLabel: a String temporary would die before drawMenu() read it.
      {"Identity", badgeId.length() ? badgeId.c_str() : "not ready",
       identity::ready() ? theme::PURPLE : theme::MUTED},
      {"Display", "", theme::MUTED},
      {"LEDs", "", theme::MUTED},
      {"Device info", "", theme::MUTED},
      {"Console", "", theme::MUTED},
  };
  drawMenu("Settings", items, SETTINGS_ITEMS, "SELECT open   CANCEL back");
}

void updateSettings() {
  if (moveCursor(SETTINGS_ITEMS)) repaint();
  if (buttons::pressed(BTN_A)) {
    switch (sCursor) {
      case 0: go(Screen::Wifi); break;
      case 1: go(Screen::Bluetooth); break;
      case 2: go(Screen::Espnow); break;
      case 3: go(Screen::Push); break;
      case 4: go(Screen::Broker); break;
      case 5: go(Screen::Identity); break;
      case 6: go(Screen::Display); break;
      case 7: go(Screen::Leds); break;
      case 8: go(Screen::Info); break;
      case 9: go(Screen::Console); break;
      default: break;
    }
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Launcher);
}

// ---------------------------------------------------------------------------
// Wi-Fi
//
// There is no keyboard on this badge, so a WPA passphrase cannot be typed in.
// Open networks connect from here directly; a secured one is set up by starting
// the hotspot and using the web UI from a phone, which is the flow the whole
// push system is built around anyway.
// ---------------------------------------------------------------------------

void drawWifi() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("Wi-Fi");

  const int actionCount = WIFI_ACTIONS;
  const int networkCount = wifi_mgr::scanning() ? 0 : wifi_mgr::scanResultCount();
  clampCursor(actionCount + networkCount);

  // Status block above the list.
  char line[64];
  snprintf(line, sizeof(line), "%s", wifi_mgr::statusText());
  display::text(line, 10, LIST_TOP, wifi_mgr::connected() ? theme::GREEN : theme::MUTED, 1);
  if (wifi_mgr::connected()) {
    snprintf(line, sizeof(line), "%s  %s", wifi_mgr::ssid().c_str(),
             wifi_mgr::ip().toString().c_str());
    display::text(line, 10, LIST_TOP + 14, theme::TEXT, 1);
    drawSignalBars(display::width() - 34, LIST_TOP, wifi_mgr::rssi());
  }

  const int top = LIST_TOP + 34;
  // Held in named locals: a String temporary built inside the initialiser dies
  // at the end of this statement and the array would outlive its c_str().
  const String saved = settings::wifiSsid();
  const bool enterprise = settings::wifiIsEnterprise();
  const String savedLabel =
      saved.length() ? (enterprise ? saved + " (EAP)" : saved) : String("-");

  const MenuItem actions[] = {
      {"Scan for networks", wifi_mgr::scanning() ? "scanning..." : "", theme::MUTED},
      // The saved profile, re-joinable in one press. This is the row that
      // matters at a conference: enterprise credentials are set up once from a
      // phone and then rejoined from the badge all week.
      {"Connect saved network", savedLabel.c_str(),
       saved.length() ? (enterprise ? theme::PURPLE : theme::MUTED) : theme::MUTED},
      {"Start hotspot", wifi_mgr::mode() == wifi_mgr::Mode::AccessPoint ? "on" : "",
       theme::GREEN},
      {"Disconnect", "", theme::MUTED},
      {"Forget saved network", savedLabel.c_str(), theme::MUTED},
  };

  const int rows = (display::height() - top - FOOTER_HEIGHT) / ROW_HEIGHT;
  for (int i = 0; i < rows && (sScroll + i) < actionCount + networkCount; ++i) {
    const int index = sScroll + i;
    const int y = top + i * ROW_HEIGHT;
    const bool selected = index == sCursor;

    if (selected) {
      canvas.fillRoundRect(4, y - 2, display::width() - 8, ROW_HEIGHT - 2, 4, theme::PANEL_2);
      canvas.fillRect(4, y - 2, 3, ROW_HEIGHT - 2, theme::PURPLE);
    }

    if (index < actionCount) {
      display::text(actions[index].label, 12, y + 3, selected ? theme::WHITE : theme::TEXT, 1);
      if (actions[index].value[0]) {
        display::textRight(actions[index].value, display::width() - 12, y + 3,
                           actions[index].valueColor, 1);
      }
    } else {
      const int network = index - actionCount;
      const bool locked = wifi_mgr::scanEncrypted(network);
      String label = wifi_mgr::scanSsid(network);
      if (locked) label = "* " + label;  // secured: cannot be joined from here
      display::text(label.c_str(), 12, y + 3,
                    selected ? theme::WHITE : (locked ? theme::MUTED : theme::TEXT), 1);
      drawSignalBars(display::width() - 34, y + 4, wifi_mgr::scanRssi(network));
    }
  }

  footer(sCursor >= actionCount && wifi_mgr::scanEncrypted(sCursor - actionCount)
             ? "* needs a password - use the hotspot"
             : "SELECT join   CANCEL back");
}

void updateWifi() {
  const int actionCount = WIFI_ACTIONS;
  const int total = actionCount + (wifi_mgr::scanning() ? 0 : wifi_mgr::scanResultCount());
  if (moveCursor(total)) repaint();

  if (buttons::pressed(BTN_A)) {
    if (sCursor == 0) {
      wifi_mgr::startScan();
    } else if (sCursor == 1) {
      // Re-join whatever is saved, personal or enterprise. The enterprise path
      // reads its CA out of the cert store and refuses if it has gone missing.
      const String saved = settings::wifiSsid();
      if (saved.length() == 0) {
        badge_log::tagf("ui", "no saved network - set one up from the web UI");
      } else if (settings::wifiIsEnterprise()) {
        if (!wifi_mgr::connectEnterprise(saved, settings::enterpriseConfig(), false)) {
          badge_log::tagf("ui", "enterprise connect refused - see Console");
        }
      } else {
        wifi_mgr::connect(saved, settings::wifiPassword(), false);
      }
    } else if (sCursor == 2) {
      wifi_mgr::startAccessPoint();
      push_server::begin();
    } else if (sCursor == 3) {
      wifi_mgr::disconnect();
      push_server::stop();
    } else if (sCursor == 4) {
      settings::forgetWifi();
    } else {
      const int network = sCursor - actionCount;
      if (!wifi_mgr::scanEncrypted(network)) {
        wifi_mgr::connect(wifi_mgr::scanSsid(network), "", true);
      } else {
        badge_log::tagf("ui", "'%s' is secured - set it up from the web UI",
                        wifi_mgr::scanSsid(network).c_str());
      }
    }
    repaint();
    return;
  }

  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------

void drawBluetooth() {
  const String address = ble_mgr::address();
  const MenuItem items[] = {
      {"Bluetooth LE", onOff(ble_mgr::enabled()), onOffColor(ble_mgr::enabled())},
      {"Connected", onOff(ble_mgr::connected()), onOffColor(ble_mgr::connected())},
      {"Enable at boot", onOff(settings::bleEnabledAtBoot()),
       onOffColor(settings::bleEnabledAtBoot())},
  };
  drawMenu("Bluetooth", items, 3, "SELECT toggle   CANCEL back");

  display::text("Nordic UART service", 10, display::height() - FOOTER_HEIGHT - 34, theme::MUTED, 1);
  display::text(address.length() ? address.c_str() : "6e400001-b5a3-f393-e0a9-e50e24dcca9e", 10,
                display::height() - FOOTER_HEIGHT - 20, theme::MUTED, 1);
}

void updateBluetooth() {
  if (moveCursor(3)) repaint();
  if (buttons::pressed(BTN_A)) {
    if (sCursor == 0) {
      if (ble_mgr::enabled()) {
        ble_mgr::end();
      } else {
        ble_mgr::begin(settings::deviceName());
      }
    } else if (sCursor == 2) {
      settings::setBleEnabledAtBoot(!settings::bleEnabledAtBoot());
    }
    repaint();
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// ESP-NOW radar - the signal view
// ---------------------------------------------------------------------------

void drawEspnow() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("ESP-NOW");

  char header[64];
  snprintf(header, sizeof(header), "%s  channel %u  %u peer%s",
           espnow_mgr::enabled() ? "on" : "off", (unsigned)espnow_mgr::channel(),
           (unsigned)espnow_mgr::peerCount(), espnow_mgr::peerCount() == 1 ? "" : "s");
  display::text(header, 10, LIST_TOP, espnow_mgr::enabled() ? theme::GREEN : theme::MUTED, 1);

  if (!espnow_mgr::enabled()) {
    display::textCentered("Press SELECT to turn ESP-NOW on", display::width() / 2, 90, theme::MUTED, 1);
    footer("SELECT toggle   CANCEL back");
    return;
  }

  if (espnow_mgr::peerCount() == 0) {
    display::textCentered("Listening for other badges...", display::width() / 2, 90, theme::MUTED,
                          1);
    display::textCentered("They must be on the same channel", display::width() / 2, 108,
                          theme::MUTED, 1);
  }

  const int top = LIST_TOP + 20;
  const int rows = (display::height() - top - FOOTER_HEIGHT) / ROW_HEIGHT;
  const uint32_t now = millis();

  for (int i = 0; i < rows && (size_t)i < espnow_mgr::peerCount(); ++i) {
    const espnow_mgr::Peer *peer = espnow_mgr::peerAt((size_t)i);
    if (peer == nullptr) continue;
    const int y = top + i * ROW_HEIGHT;

    display::text(peer->name, 12, y + 3, theme::TEXT, 1);

    char detail[40];
    snprintf(detail, sizeof(detail), "%ddBm %lus", peer->rssi,
             (unsigned long)((now - peer->lastSeenMs) / 1000));
    display::textRight(detail, display::width() - 44, y + 3, theme::MUTED, 1);
    drawSignalBars(display::width() - 34, y + 4, peer->rssi);
  }

  footer("SELECT toggle   left/right channel   CANCEL back");
}

void updateEspnow() {
  if (buttons::pressed(BTN_A)) {
    if (espnow_mgr::enabled()) {
      espnow_mgr::end();
      settings::setEspnowEnabledAtBoot(false);
    } else {
      espnow_mgr::begin(settings::espnowChannel());
      settings::setEspnowEnabledAtBoot(true);
    }
    repaint();
    return;
  }

  // Changing channel means tearing ESP-NOW down and bringing it back up; there
  // is no way to move an initialised peer set across channels.
  int channel = settings::espnowChannel();
  if (buttons::pressed(BTN_LEFT)) --channel;
  if (buttons::pressed(BTN_RIGHT)) ++channel;
  if (channel != settings::espnowChannel() && channel >= 1 && channel <= 13) {
    settings::setEspnowChannel((uint8_t)channel);
    if (espnow_mgr::enabled()) {
      espnow_mgr::end();
      espnow_mgr::begin((uint8_t)channel);
    }
    repaint();
  }

  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// App push
// ---------------------------------------------------------------------------

void drawPush() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("App push");

  const bool up = push_server::running() && wifi_mgr::connected();
  display::text(up ? "Web UI ready" : "Wi-Fi is off", 10, LIST_TOP,
                up ? theme::GREEN : theme::MUTED, 1);

  if (up) {
    display::text(("http://" + wifi_mgr::ip().toString() + "/").c_str(), 10, LIST_TOP + 16,
                  theme::TEXT, 1);
    display::text(("http://" + settings::deviceName() + ".local/").c_str(), 10, LIST_TOP + 30,
                  theme::MUTED, 1);
  } else {
    display::text("Settings > Wi-Fi to connect", 10, LIST_TOP + 16, theme::MUTED, 1);
    display::text("or start the hotspot", 10, LIST_TOP + 30, theme::MUTED, 1);
  }

  // The pairing code, big enough to read across a table.
  const int codeY = LIST_TOP + 54;
  canvas.fillRoundRect(10, codeY, display::width() - 20, 40, 6, theme::PANEL);
  canvas.drawRoundRect(10, codeY, display::width() - 20, 40, 6, theme::BORDER);
  display::text("PAIRING CODE", 20, codeY + 6, theme::MUTED, 1);
  display::textRight(settings::pairingCode().c_str(), display::width() - 20, codeY + 18,
                     settings::pushRequiresPairing() ? theme::GREEN : theme::MUTED, 2);

  const MenuItem items[] = {
      {"Require pairing code", onOff(settings::pushRequiresPairing()),
       onOffColor(settings::pushRequiresPairing())},
      {"New pairing code", "", theme::MUTED},
      {"BLE push", ble_mgr::enabled() ? "advertising" : "off", onOffColor(ble_mgr::enabled())},
  };

  const int top = codeY + 50;
  for (int i = 0; i < 3; ++i) {
    const int y = top + i * ROW_HEIGHT;
    const bool selected = i == sCursor;
    if (selected) {
      canvas.fillRoundRect(4, y - 2, display::width() - 8, ROW_HEIGHT - 2, 4, theme::PANEL_2);
      canvas.fillRect(4, y - 2, 3, ROW_HEIGHT - 2, theme::PURPLE);
    }
    display::text(items[i].label, 12, y + 3, selected ? theme::WHITE : theme::TEXT, 1);
    if (items[i].value[0]) {
      display::textRight(items[i].value, display::width() - 12, y + 3, items[i].valueColor, 1);
    }
  }

  footer("SELECT choose   CANCEL back");
}

void updatePush() {
  if (moveCursor(3)) repaint();
  if (buttons::pressed(BTN_A)) {
    if (sCursor == 0) {
      settings::setPushRequiresPairing(!settings::pushRequiresPairing());
    } else if (sCursor == 1) {
      settings::regeneratePairingCode();
      leds::pulse(0x99, 0x45, 0xFF, 400);
    } else if (sCursor == 2) {
      if (ble_mgr::enabled()) {
        ble_mgr::end();
      } else {
        ble_mgr::begin(settings::deviceName());
      }
    }
    repaint();
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// Identity
//
// The badge ID is the one string on this device a stranger has to read off the
// screen and type somewhere else, so it is laid out like the pairing code on
// the Push screen rather than as another settings row.
// ---------------------------------------------------------------------------

void drawIdentity() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("Identity");

  const bool ready = identity::ready();
  const bool secure = identity::source() == identity::Source::SecureElement;

  const int panelY = LIST_TOP;
  canvas.fillRoundRect(10, panelY, display::width() - 20, 52, 6, theme::PANEL);
  canvas.drawRoundRect(10, panelY, display::width() - 20, 52, 6, theme::BORDER);
  display::text("BADGE ID", 20, panelY + 6, theme::MUTED, 1);
  display::textCentered(ready ? identity::badgeId().c_str() : "--------", display::width() / 2,
                        panelY + 22, ready ? theme::GREEN : theme::MUTED, 3);

  int y = panelY + 62;
  display::text("key lives in", 10, y, theme::MUTED, 1);
  // Green only for the secure element: which of the two paths is in use is a
  // real difference in what the badge can promise, and colouring both the same
  // would quietly hide a dead SE050.
  display::textRight(identity::sourceName(), display::width() - 10, y,
                     secure ? theme::GREEN : theme::WARN, 1);
  y += 16;
  display::text(identity::status().c_str(), 10, y, theme::MUTED, 1);

  y += 22;
  display::text("public key", 10, y, theme::MUTED, 1);
  y += 14;
  // Base58 is 43-44 characters and the panel is 320px, so it wraps once. Split
  // by hand rather than letting it run off the right edge - a half-shown key is
  // worse than useless for checking against a wallet.
  const String key = identity::publicKeyBase58();
  constexpr int PER_LINE = 32;
  for (int offset = 0; offset < (int)key.length(); offset += PER_LINE) {
    display::text(key.substring(offset, offset + PER_LINE).c_str(), 10, y, theme::MUTED, 1);
    y += 13;
  }

  // row() lays out from LIST_TOP, which the badge-ID panel already owns, so the
  // single action is drawn directly at the bottom instead.
  const int actionY = display::height() - FOOTER_HEIGHT - ROW_HEIGHT;
  canvas.fillRoundRect(4, actionY, display::width() - 8, ROW_HEIGHT - 2, 4, theme::PANEL_2);
  canvas.fillRect(4, actionY, 3, ROW_HEIGHT - 2, theme::PURPLE);
  display::text("New identity", 12, actionY + 5, theme::WHITE, 1);
  display::textRight("changes the badge ID", display::width() - 12, actionY + 5, theme::WARN, 1);

  footer("SELECT new identity   CANCEL back");
}

void updateIdentity() {
  if (buttons::pressed(BTN_A)) {
    go(Screen::IdentityNew);
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

void drawIdentityNew() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("New identity");
  canvas.fillRect(0, display::STATUS_BAR_HEIGHT + 2, display::width(), 3, theme::WARN);

  int y = LIST_TOP + 6;
  display::text("This throws the keypair away and", 10, y, theme::TEXT, 1);
  y += 14;
  display::text("makes a new one. It cannot be undone.", 10, y, theme::TEXT, 1);
  y += 22;
  display::text("The badge ID changes, so:", 10, y, theme::MUTED, 1);
  y += 16;
  display::text("- the App Store loses this badge", 10, y, theme::MUTED, 1);
  y += 14;
  display::text("- anyone holding the old ID must", 10, y, theme::MUTED, 1);
  y += 14;
  display::text("  be given the new one", 10, y, theme::MUTED, 1);
  y += 14;
  display::text("- installed apps are left alone", 10, y, theme::MUTED, 1);

  y += 22;
  const String id = identity::badgeId();
  display::text(id.length() ? (id + "  ->  ?").c_str() : "no identity yet", 10, y, theme::WARN, 1);

  footer("SELECT confirm   CANCEL back");
}

void updateIdentityNew() {
  if (buttons::pressed(BTN_A)) {
    // Blocks for a second or two on both key paths. The warning is still on the
    // panel while it runs, which is a better thing to be looking at than a
    // half-drawn menu, so it is not worth splitting across frames.
    const bool ok = identity::regenerate();
    // The stored token was issued against the old public key. Dropping it here
    // means the next broker tick re-registers under the new badge ID rather
    // than collecting 401s nobody would think to go looking for.
    broker::forget();
    badge_log::tagf("ui", "identity regenerated: %s", ok ? "ok" : "failed");
    if (ok) {
      leds::pulse(0x99, 0x45, 0xFF, 700);
    } else {
      leds::pulse(0xFF, 0x45, 0x45, 700);
    }
    go(Screen::Identity);
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Identity);
}

// ---------------------------------------------------------------------------
// App store (the broker client)
// ---------------------------------------------------------------------------

void drawBroker() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("App store");

  const bool healthy = broker::state() == broker::State::Idle ||
                       broker::state() == broker::State::Offered ||
                       broker::state() == broker::State::Installing;

  int y = LIST_TOP;
  display::text(broker::stateText(), 10, y,
                broker::state() == broker::State::Error ? theme::ERR
                : healthy                               ? theme::GREEN
                                                        : theme::MUTED,
                1);
  display::textRight(broker::registered() ? "registered" : "not registered", display::width() - 10,
                     y, broker::registered() ? theme::GREEN : theme::MUTED, 1);

  y += 18;
  const String address = broker::url();
  display::text(address.length() ? address.c_str() : "no address set", 10, y,
                address.length() ? theme::TEXT : theme::MUTED, 1);

  y += 16;
  const String error = broker::lastError();
  if (error.length()) display::text(error.substring(0, 50).c_str(), 10, y, theme::ERR, 1);

  // Same reason the Wi-Fi screen cannot take a passphrase: there is no keyboard
  // on this badge. Pointing at the web UI is the whole answer, so it is on
  // screen rather than in a manual.
  y += 22;
  display::text("The address is set from the web UI,", 10, y, theme::MUTED, 1);
  y += 14;
  display::text("not from here - no keyboard.", 10, y, theme::MUTED, 1);
  y += 14;
  if (wifi_mgr::connected()) {
    display::text(("http://" + wifi_mgr::ip().toString() + "/").c_str(), 10, y, theme::PURPLE, 1);
  } else {
    display::text("Settings > Wi-Fi, then browse to the badge", 10, y, theme::MUTED, 1);
  }

  const MenuItem items[] = {
      {"App store", onOff(broker::enabled()), onOffColor(broker::enabled())},
      {"Forget registration", broker::registered() ? "re-register" : "", theme::MUTED},
  };

  const int top = display::height() - FOOTER_HEIGHT - 2 * ROW_HEIGHT - 4;
  for (int i = 0; i < 2; ++i) {
    const int rowY = top + i * ROW_HEIGHT;
    const bool selected = i == sCursor;
    if (selected) {
      canvas.fillRoundRect(4, rowY - 2, display::width() - 8, ROW_HEIGHT - 2, 4, theme::PANEL_2);
      canvas.fillRect(4, rowY - 2, 3, ROW_HEIGHT - 2, theme::PURPLE);
    }
    display::text(items[i].label, 12, rowY + 3, selected ? theme::WHITE : theme::TEXT, 1);
    if (items[i].value[0]) {
      display::textRight(items[i].value, display::width() - 12, rowY + 3, items[i].valueColor, 1);
    }
  }

  footer("SELECT choose   CANCEL back");
}

void updateBroker() {
  if (moveCursor(2)) repaint();
  if (buttons::pressed(BTN_A)) {
    if (sCursor == 0) {
      broker::setEnabled(!broker::enabled());
    } else {
      broker::forget();
    }
    repaint();
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// The offer prompt
//
// The one screen in the shell that appears without being asked for, and the
// only thing standing between "anyone who knows a badge ID" and code on this
// badge's flash. It shows what is being offered and by whom, and the footer
// spells out both answers rather than leaving A and B to be guessed at.
// ---------------------------------------------------------------------------

// Fixed rows above the script list.
constexpr int OFFER_HEADER = 40;
constexpr int OFFER_ROW_HEIGHT = 28;

inline int offerRows() {
  return (display::height() - LIST_TOP - OFFER_HEADER - FOOTER_HEIGHT) / OFFER_ROW_HEIGHT;
}

void drawOffer() {
  auto &canvas = display::canvas();
  const broker::Offer &offer = broker::offer();

  canvas.fillScreen(theme::BG);
  display::statusBar("App store");

  // The brand ramp, as a band across the top: this frame arrived on its own and
  // has to read as an event rather than as another settings page.
  for (int x = 0; x < display::width(); ++x) {
    canvas.drawFastVLine(x, display::STATUS_BAR_HEIGHT + 2, 3,
                         display::gradient565Lut(x, display::width()));
  }

  clampCursor(offer.count);

  display::text(offer.repo.length() ? offer.repo.c_str() : "unknown source", 10, LIST_TOP + 2,
                theme::WHITE, 1);

  char sub[72];
  snprintf(sub, sizeof(sub), "%s%s%s  %u script%s",
           offer.ref.length() ? offer.ref.c_str() : "-", offer.sender.length() ? " via " : "",
           offer.sender.length() ? offer.sender.c_str() : "", (unsigned)offer.count,
           offer.count == 1 ? "" : "s");
  display::text(sub, 10, LIST_TOP + 18, theme::MUTED, 1);

  const int top = LIST_TOP + OFFER_HEADER;
  const int rows = offerRows();
  for (int i = 0; i < rows && (sScroll + i) < offer.count; ++i) {
    const int index = sScroll + i;
    const broker::ScriptInfo &script = offer.scripts[index];
    const int y = top + i * OFFER_ROW_HEIGHT;
    const bool selected = index == sCursor;

    if (selected) {
      canvas.fillRoundRect(4, y - 3, display::width() - 8, OFFER_ROW_HEIGHT - 3, 4, theme::PANEL_2);
      canvas.fillRect(4, y - 3, 3, OFFER_ROW_HEIGHT - 3, theme::PURPLE);
    }

    display::text(script.name.c_str(), 12, y, selected ? theme::WHITE : theme::TEXT, 1);

    char size[16];
    if (script.bytes >= 1024) {
      snprintf(size, sizeof(size), "%u KB", (unsigned)(script.bytes / 1024));
    } else {
      snprintf(size, sizeof(size), "%u B", (unsigned)script.bytes);
    }
    display::textRight(size, display::width() - 12, y, theme::MUTED, 1);

    // The description is the author's own first `--` line - the closest thing to
    // an answer to "what does this do" before it runs.
    const String detail = script.description.length() ? script.description : script.file;
    display::text(detail.substring(0, 48).c_str(), 12, y + 12, theme::MUTED, 1);
  }

  if (offer.count > rows) {
    char position[16];
    snprintf(position, sizeof(position), "%d/%u", sCursor + 1, (unsigned)offer.count);
    display::textRight(position, display::width() - 8, display::height() - FOOTER_HEIGHT - 12,
                       theme::MUTED, 1);
  }

  // Not the usual muted hint: this footer is the decision.
  const int footerY = display::height() - FOOTER_HEIGHT;
  canvas.fillRect(0, footerY, display::width(), FOOTER_HEIGHT, theme::HEADER);
  display::text("SELECT install", 8, footerY + 5, theme::GREEN, 1);
  display::textRight("CANCEL decline", display::width() - 8, footerY + 5, theme::ERR, 1);
}

void updateOffer() {
  // The offer can go away underneath the prompt - it expires after an hour and
  // the broker stops serving it - so the screen has to be able to leave on its
  // own rather than stranding the wearer on a dead A press.
  if (!broker::hasOffer()) {
    if (broker::installing()) {
      sInstallWaitStart = millis();
      go(Screen::OfferInstalling);
    } else {
      go(Screen::Launcher);
    }
    return;
  }

  // The prompt can appear on the exact tick the wearer pressed SELECT for
  // whatever was on screen before (the launcher, a settings row). Those edge
  // masks were computed before the offer existed, so they are not consent -
  // swallow this tick's edges and require a fresh press to accept or decline.
  if (sOfferJustRaised) {
    sOfferJustRaised = false;
    return;
  }

  if (moveCursor(broker::offer().count)) repaint();

  if (buttons::pressed(BTN_A)) {
    broker::accept();
    sInstallWaitStart = millis();
    go(Screen::OfferInstalling);
    return;
  }
  if (buttons::pressed(BTN_B)) {
    broker::decline();
    go(Screen::Launcher);
  }
}

void drawOfferInstalling() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("App store");

  const String result = broker::lastResult();
  const bool done = result.length() > 0;

  if (done) {
    const bool clean = broker::failedCount() == 0;
    display::textCentered(clean ? "Installed" : "Finished with errors", display::width() / 2,
                          LIST_TOP + 20, clean ? theme::GREEN : theme::WARN, 2);
    display::textCentered(result.c_str(), display::width() / 2, LIST_TOP + 54, theme::TEXT, 1);
    display::textCentered("The launcher has them now.", display::width() / 2, LIST_TOP + 76,
                          theme::MUTED, 1);
    footer("SELECT or CANCEL   back to the launcher");
    return;
  }

  display::textCentered("Installing", display::width() / 2, LIST_TOP + 16, theme::WHITE, 2);

  const uint8_t percent = broker::installProgress();
  const int barX = 24;
  const int barY = LIST_TOP + 56;
  const int barW = display::width() - 48;
  constexpr int barH = 14;
  canvas.drawRoundRect(barX, barY, barW, barH, 5, theme::BORDER);
  const int fill = ((int)percent * (barW - 4)) / 100;
  for (int x = 0; x < fill; ++x) {
    canvas.drawFastVLine(barX + 2 + x, barY + 2, barH - 4,
                         display::gradient565Lut(x, barW - 4));
  }

  char line[48];
  snprintf(line, sizeof(line), "%u ok  %u failed", (unsigned)broker::installedCount(),
           (unsigned)broker::failedCount());
  display::textCentered(line, display::width() / 2, barY + 26, theme::MUTED, 1);

  // Once the install has clearly hung, tell the wearer there is a way out rather
  // than leaving the sha256 reassurance up on a screen that is going nowhere.
  if ((millis() - sInstallWaitStart) >= INSTALL_ESCAPE_MS) {
    footer("stuck? CANCEL to give up");
  } else {
    footer("verifying sha256 before each write");
  }
}

void updateOfferInstalling() {
  if (broker::lastResult().length() == 0) {
    // Still working. A stalled install must not trap the badge here forever -
    // the long-hold-B force-quit only rescues Lua apps, not shell screens - so
    // once the install has clearly hung, let B abandon it back to the launcher.
    // The broker keeps whatever state it has; a result landing later is
    // harmless once we are away.
    if (buttons::pressed(BTN_B) && (millis() - sInstallWaitStart) >= INSTALL_ESCAPE_MS) {
      go(Screen::Launcher);
    }
    return;
  }
  if (buttons::pressed(BTN_A) || buttons::pressed(BTN_B)) {
    broker::clearResult();
    // Straight to the launcher, where the install already refreshed the
    // catalogue - the whole point of accepting was to run the thing.
    go(Screen::Launcher);
  }
}

// ---------------------------------------------------------------------------
// Display / LEDs
// ---------------------------------------------------------------------------

void drawSlider(const char *title, const char *label, uint8_t value, const char *hint) {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar(title);

  display::text(label, 12, LIST_TOP + 10, theme::TEXT, 1);

  char percent[8];
  snprintf(percent, sizeof(percent), "%d%%", (value * 100) / 255);
  display::textRight(percent, display::width() - 12, LIST_TOP + 10, theme::WHITE, 1);

  const int barX = 12;
  const int barY = LIST_TOP + 32;
  const int barW = display::width() - 24;
  constexpr int barH = 14;
  canvas.drawRoundRect(barX, barY, barW, barH, 5, theme::BORDER);
  const int fill = (value * (barW - 4)) / 255;
  for (int x = 0; x < fill; ++x) {
    canvas.drawFastVLine(barX + 2 + x, barY + 2, barH - 4,
                         display::gradient565Lut(x, barW - 4));
  }
  footer(hint);
}

void updateDisplayScreen() {
  const uint8_t before = settings::brightness();
  const uint8_t after = adjust(before, 8);
  if (after != before) {
    // Applied live so the value being chosen is the value being seen. A floor
    // of 8 keeps left-held from turning the screen off entirely, which would
    // look exactly like a crash.
    const uint8_t clamped = after < 8 ? 8 : after;
    settings::setBrightness(clamped);
    display::setBrightness(clamped);
    repaint();
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

void updateLedScreen() {
  const uint8_t before = settings::ledBrightness();
  const uint8_t after = adjust(before, 8);
  if (after != before) {
    settings::setLedBrightness(after);
    leds::setBrightness(after);
    repaint();
  }
  if (buttons::pressed(BTN_A)) leds::playBoot();  // preview
  if (buttons::pressed(BTN_B)) {
    leds::stopAnimation();
    leds::off();
    go(Screen::Settings);
  }
}

// ---------------------------------------------------------------------------
// Device info
// ---------------------------------------------------------------------------

void drawInfo() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("Device info");

  char line[80];
  int y = LIST_TOP;
  const int step = 15;

  auto field = [&](const char *label, const char *value, uint16_t color) {
    display::text(label, 10, y, theme::MUTED, 1);
    display::textRight(value, display::width() - 10, y, color, 1);
    y += step;
  };

  field("firmware", SOLANA_OS_NAME " " SOLANA_OS_VERSION, theme::TEXT);
  field("device", settings::deviceName().c_str(), theme::TEXT);

  snprintf(line, sizeof(line), "%s rev%u @%uMHz", ESP.getChipModel(), ESP.getChipRevision(),
           (unsigned)ESP.getCpuFreqMHz());
  field("chip", line, theme::TEXT);

  snprintf(line, sizeof(line), "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
  field("heap", line, theme::TEXT);

  snprintf(line, sizeof(line), "%u / %u KB", (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)(ESP.getPsramSize() / 1024));
  field("psram free", line, theme::TEXT);

  snprintf(line, sizeof(line), "%u / %u KB", (unsigned)(app_store::usedBytes() / 1024),
           (unsigned)(app_store::totalBytes() / 1024));
  field("storage", line, theme::TEXT);

  snprintf(line, sizeof(line), "%.2fV  %d%%%s", power::volts(), (int)power::percent(),
           power::charging() ? " chg" : "");
  field("battery", line, power::percent() < 15 ? theme::ERR : theme::TEXT);

  field("i2c bus", badge_i2c::down() ? "held low" : "ok",
        badge_i2c::down() ? theme::ERR : theme::GREEN);
  field("buttons", buttons::present() ? "TCA9534 ok" : "not found",
        buttons::present() ? theme::GREEN : theme::ERR);
  field("se050", se050::present() ? "ATR ok" : "no answer",
        se050::present() ? theme::GREEN : theme::WARN);
  field("wifi mac", wifi_mgr::macAddress().c_str(), theme::TEXT);

  snprintf(line, sizeof(line), "%lus", (unsigned long)(millis() / 1000));
  field("uptime", line, theme::TEXT);

  footer("SELECT re-scan I2C   CANCEL back");
}

void updateInfo() {
  // The one place a wedged bus can be retried by hand after recovery gave up.
  if (buttons::pressed(BTN_A)) {
    badge_i2c::retry();
    buttons::retry();
    se050::test();
    badge_i2c::scan();
    repaint();
    return;
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// Console - the log ring, newest at the bottom
// ---------------------------------------------------------------------------

void drawConsole() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BLACK);
  display::statusBar("Console");

  const int lineHeight = 11;
  const int visible = (display::height() - LIST_TOP - FOOTER_HEIGHT) / lineHeight;
  const int total = (int)badge_log::lineCount();
  const int first = total > visible ? total - visible - sScroll : 0;

  for (int i = 0; i < visible; ++i) {
    const int index = first + i;
    if (index < 0 || index >= total) continue;
    const char *text = badge_log::line((size_t)index);
    // Errors in red so a failure is findable without reading every line.
    const bool bad = strstr(text, "error") || strstr(text, "failed") || strstr(text, "FATAL");
    display::text(text, 6, LIST_TOP + i * lineHeight, bad ? theme::ERR : theme::MUTED, 1);
  }

  footer("up/down scroll   CANCEL back");
}

void updateConsole() {
  const int total = (int)badge_log::lineCount();
  const int visible = (display::height() - LIST_TOP - FOOTER_HEIGHT) / 11;
  const int maxScroll = total > visible ? total - visible : 0;

  if (buttons::repeated(BTN_UP) && sScroll < maxScroll) {
    ++sScroll;
    repaint();
  }
  if (buttons::repeated(BTN_DOWN) && sScroll > 0) {
    --sScroll;
    repaint();
  }
  if (buttons::pressed(BTN_B)) go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// App error
// ---------------------------------------------------------------------------

void drawAppError() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("App stopped");

  canvas.fillRect(0, display::STATUS_BAR_HEIGHT + 2, display::width(), 3, theme::ERR);

  // Wrap the message by hand: the traceback is one long line and the panel is
  // 320px wide, so letting it run off the edge would hide the useful part.
  const int charsPerLine = (display::width() - 20) / 6;
  int y = LIST_TOP + 6;
  int offset = 0;
  const int maxLines = (display::height() - y - FOOTER_HEIGHT - 4) / 12;

  for (int line = 0; line < maxLines && offset < (int)sError.length(); ++line) {
    String chunk = sError.substring(offset, offset + charsPerLine);
    const int newline = chunk.indexOf('\n');
    if (newline >= 0) {
      chunk = chunk.substring(0, newline);
      offset += newline + 1;
    } else {
      offset += charsPerLine;
    }
    display::text(chunk.c_str(), 10, y, line == 0 ? theme::WHITE : theme::MUTED, 1);
    y += 12;
  }

  footer("SELECT retry   CANCEL launcher");
}

void updateAppError() {
  // currentApp() is empty by the time we get here - the app has already been
  // torn down - so the retry target comes from runtime::lastApp().
  if (buttons::pressed(BTN_A)) {
    const String retry = runtime::lastApp();
    if (retry.length() && app_store::exists(retry)) {
      runtime::clearError();
      runtime::requestLaunch(retry);
    }
    return;
  }
  if (buttons::pressed(BTN_B)) {
    runtime::clearError();
    go(Screen::Launcher);
  }
}

// ---------------------------------------------------------------------------
// Delete an installed app
// ---------------------------------------------------------------------------

void drawAppDelete() {
  auto &canvas = display::canvas();
  canvas.fillScreen(theme::BG);
  display::statusBar("Delete app");

  canvas.fillRect(0, display::STATUS_BAR_HEIGHT + 2, display::width(), 3, theme::ERR);

  display::textCentered("Delete this app?", display::width() / 2, LIST_TOP + 14, theme::WHITE, 1);
  display::textCentered(sDeleteName.c_str(), display::width() / 2, LIST_TOP + 40, theme::WHITE, 2);

  app_store::Info info;
  if (app_store::byId(sDeleteId, info)) {
    char detail[64];
    snprintf(detail, sizeof(detail), "%s  ·  %u KB", info.id.c_str(),
             (unsigned)((info.sizeBytes + 1023) / 1024));
    display::textCentered(detail, display::width() / 2, LIST_TOP + 72, theme::MUTED, 1);
  }

  display::textCentered("Its files and saved data go with it.", display::width() / 2,
                        LIST_TOP + 98, theme::MUTED, 1);
  display::textCentered("This cannot be undone.", display::width() / 2, LIST_TOP + 116,
                        theme::WARN, 1);

  // Same shape as the app-store offer: this footer is the decision, so it gets
  // colour rather than the usual muted hint.
  const int footerY = display::height() - FOOTER_HEIGHT;
  canvas.fillRect(0, footerY, display::width(), FOOTER_HEIGHT, theme::HEADER);
  display::text("SELECT delete", 8, footerY + 5, theme::ERR, 1);
  display::textRight("CANCEL keep", display::width() - 8, footerY + 5, theme::GREEN, 1);
}

void updateAppDelete() {
  if (buttons::pressed(BTN_A)) {
    // The app can have gone away underneath the prompt - a push can remove it,
    // and byId() is the only thing that knows. Treat "already gone" as success
    // rather than as an error the wearer has to make sense of.
    const bool removed = !app_store::exists(sDeleteId) || app_store::removeApp(sDeleteId);
    app_store::refresh();

    if (removed) {
      badge_log::tagf("os", "deleted app '%s'", sDeleteId.c_str());
      leds::pulse(0xFF, 0x45, 0x45, 500);
    } else {
      badge_log::tagf("os", "could not delete app '%s'", sDeleteId.c_str());
      sError = "Could not delete " + sDeleteName;
      sDeleteId = "";
      sDeleteName = "";
      sScreen = Screen::AppError;
      sCursor = 0;
      sScroll = 0;
      repaint();
      return;
    }

    sDeleteId = "";
    sDeleteName = "";
    // The launcher is shorter than it was; clampCursor in drawLauncher pulls
    // the cursor back into range. The pulse above hands itself back to the idle
    // animation when it finishes.
    go(Screen::Launcher);
    return;
  }

  if (buttons::pressed(BTN_B)) {
    sDeleteId = "";
    sDeleteName = "";
    go(Screen::Launcher);
  }
}

}  // namespace

// ---------------------------------------------------------------------------

void begin() {
  app_store::refresh();
  sScreen = Screen::Launcher;
  repaint();
}

void showError(const String &message) {
  sError = message;
  sScreen = Screen::AppError;
  sCursor = 0;
  sScroll = 0;
  repaint();
  leds::pulse(0xFF, 0x45, 0x45, 700);
}

void onAppStopped() {
  app_store::refresh();
  if (runtime::lastError().length()) {
    showError(runtime::lastError());
  } else {
    go(Screen::Launcher);
    leds::playIdle();
  }
}

void update() {
  // The offer prompt is raised here rather than pushed from broker::update(),
  // because the main loop only calls the shell when no Lua app is running -
  // which is exactly the condition the prompt has to respect. An app keeps the
  // screen; the offer is still waiting when it exits.
  // OfferInstalling is excluded too: with a queue behind it the next offer can
  // land while the result of the last one is still on screen, and replacing it
  // unread would hide what just got written to flash.
  if (broker::hasOffer() && sScreen != Screen::Offer && sScreen != Screen::OfferInstalling) {
    go(Screen::Offer);
    // The prompt was just raised inside this same tick, so updateOffer() below
    // is about to run with the button edges that were computed before the offer
    // existed. Mark the entry so it ignores them - otherwise an in-flight SELECT
    // would be read as consent on the very first frame of the prompt.
    sOfferJustRaised = true;
    // Loud on purpose. The badge is on a lanyard and its wearer is looking at
    // whoever just sent them something, not at the screen.
    leds::pulse(0x99, 0x45, 0xFF, 900);
  }

  // Input first, so a screen switch this tick is drawn this tick rather than
  // showing one stale frame.
  switch (sScreen) {
    case Screen::Launcher: updateLauncher(); break;
    case Screen::Settings: updateSettings(); break;
    case Screen::Wifi: updateWifi(); break;
    case Screen::Bluetooth: updateBluetooth(); break;
    case Screen::Espnow: updateEspnow(); break;
    case Screen::Push: updatePush(); break;
    case Screen::Broker: updateBroker(); break;
    case Screen::Identity: updateIdentity(); break;
    case Screen::IdentityNew: updateIdentityNew(); break;
    case Screen::Display: updateDisplayScreen(); break;
    case Screen::Leds: updateLedScreen(); break;
    case Screen::Info: updateInfo(); break;
    case Screen::Console: updateConsole(); break;
    case Screen::AppError: updateAppError(); break;
    case Screen::AppDelete: updateAppDelete(); break;
    case Screen::Offer: updateOffer(); break;
    case Screen::OfferInstalling: updateOfferInstalling(); break;
  }

  // Screens whose content is genuinely dynamic repaint on a timer; the rest only
  // on input. The launcher, Push and Broker screens used to be in here too, but
  // their content barely changes - a blind full fillScreen + pushSprite at 4Hz
  // for a static menu is wasted SPI traffic and float math. The launcher's one
  // live element (app count, battery percent) is watched explicitly below.
  const bool live = sScreen == Screen::Espnow || sScreen == Screen::Info ||
                    sScreen == Screen::Wifi || sScreen == Screen::Console ||
                    sScreen == Screen::OfferInstalling;
  if (live && (millis() - sLastRepaintAt) >= LIVE_REPAINT_MS) {
    sLastRepaintAt = millis();
    sDirty = true;
  }

  // The launcher repaints only when its content actually moves: an install or
  // delete changes the app count, and the status-bar battery percent ticks down
  // over time. Both are cheap to poll and far rarer than 4Hz.
  if (sScreen == Screen::Launcher) {
    const size_t apps = app_store::count();
    const int battery = (int)power::percent();
    if (apps != sLauncherAppCount || battery != sLauncherBattery) {
      sLauncherAppCount = apps;
      sLauncherBattery = battery;
      sDirty = true;
    }
  }

  if (!sDirty) return;
  sDirty = false;

  switch (sScreen) {
    case Screen::Launcher: drawLauncher(); break;
    case Screen::Settings: drawSettings(); break;
    case Screen::Wifi: drawWifi(); break;
    case Screen::Bluetooth: drawBluetooth(); break;
    case Screen::Espnow: drawEspnow(); break;
    case Screen::Push: drawPush(); break;
    case Screen::Broker: drawBroker(); break;
    case Screen::Identity: drawIdentity(); break;
    case Screen::IdentityNew: drawIdentityNew(); break;
    case Screen::Offer: drawOffer(); break;
    case Screen::OfferInstalling: drawOfferInstalling(); break;
    case Screen::Display:
      drawSlider("Display", "Backlight", settings::brightness(), "left/right adjust   CANCEL back");
      break;
    case Screen::Leds:
      drawSlider("LEDs", "RGB brightness", settings::ledBrightness(),
                 "left/right adjust   SELECT preview   CANCEL back");
      break;
    case Screen::Info: drawInfo(); break;
    case Screen::Console: drawConsole(); break;
    case Screen::AppError: drawAppError(); break;
    case Screen::AppDelete: drawAppDelete(); break;
  }
}

}  // namespace shell
