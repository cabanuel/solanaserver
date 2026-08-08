/*
  Board and system configuration for Solana OS.

  Pin assignments are the v1 badge and match pinout/solana-badge-pinout.html
  and firmware/testkit/Badge.ino. Anything a user might reasonably want to
  retune lives here rather than being buried in a driver.
*/
#pragma once

#include <Arduino.h>

// ============================================================================
// Firmware identity
// ============================================================================
#define SOLANA_OS_NAME     "Solana OS"
#define SOLANA_OS_VERSION  "0.1.0"

// The Lua SDK reports this. Bump the minor when bindings are added, the major
// when an existing binding changes shape, so apps can gate on badge.API_VERSION.
#define SOLANA_OS_API_VERSION 1

// ============================================================================
// Pin mapping - v1 badge
// ============================================================================
constexpr int PIN_LCD_SCK  = 11;
constexpr int PIN_LCD_CS   = 12;
constexpr int PIN_LCD_DC   = 13;
constexpr int PIN_LCD_RST  = 14;
constexpr int PIN_LCD_MOSI = 15;
constexpr int PIN_LCD_MISO = 16;
constexpr int PIN_LCD_BL   = 7;

constexpr int PIN_I2C_SDA    = 10;
constexpr int PIN_I2C_SCL    = 9;
// Touch is not used, but the GT911 shares the I2C bus and must still be brought
// out of reset at boot or it clamps the bus - see badge_i2c.cpp.
constexpr int PIN_TOUCH_INT  = 5;
constexpr int PIN_TOUCH_RST  = 6;
constexpr int PIN_BUTTON_INT = 4;

constexpr int PIN_RGB           = 2;
constexpr int PIN_BATTERY       = 1;
constexpr int PIN_SE050_ISO_RST = 8;

constexpr int PIN_MIC_CLK  = 47;
constexpr int PIN_MIC_DATA = 48;

// ============================================================================
// Display
// ============================================================================
constexpr uint8_t  LCD_ROTATION   = 1;      // 1 = landscape, 320x240
constexpr bool     LCD_INVERT     = false;
constexpr uint8_t  LCD_BRIGHTNESS = 190;    // default; overridden by settings
constexpr int16_t  LCD_WIDTH      = 320;
constexpr int16_t  LCD_HEIGHT     = 240;

// Splash artwork is authored 280px wide; scale it so the logo floats centred.
constexpr int      SPLASH_TARGET_WIDTH = 176;
constexpr uint16_t SPLASH_HOLD_MS      = 1400;
constexpr uint16_t SPLASH_FADE_MS      = 320;

// ============================================================================
// I2C bus
// ============================================================================
constexpr uint32_t I2C_HZ          = 100000;
constexpr uint16_t I2C_TIMEOUT_MS  = 8;
constexpr uint8_t  TCA9534_ADDR    = 0x20;
constexpr uint8_t  SE050_ADDR      = 0x48;
constexpr uint16_t SE050_TIMEOUT_MS = 30;
constexpr uint8_t  SE050_ATR_LENGTH = 35;
// SE050 ENA/RST is an active-high enable on this board; left floating the part
// can sit in reset and never answer, so it is driven explicitly.
constexpr bool     SE050_ENABLE_ACTIVE_HIGH = true;

// ============================================================================
// Buttons
//
// The silkscreen names the six keys UP, DOWN, LEFT, RIGHT, SELECT and CANCEL,
// and those are the names any on-screen prompt should use - "A" and "B" are
// firmware words for buttons the board does not call that.
//
// What the silkscreen does *not* say is which expander bit each key hangs off.
// The P0..P5 assignment below has now been measured on hardware rather than
// guessed - see the note on it. Re-ordering these re-maps everything, since
// buttons::name(), buttons::shortName() and the Lua `badge.input` constants all
// follow whatever is set here, so a board revision that moves the wiring is a
// one-place change; re-measure with the per-press line buttons.cpp logs.
// ============================================================================
constexpr bool    BUTTON_ACTIVE_LOW  = true;
constexpr uint8_t BUTTON_COUNT       = 6;
constexpr uint8_t BUTTON_DEBOUNCE_MS = 25;
constexpr uint8_t BUTTON_POLL_MS     = 15;
// Poll interval once the I2C bus has been given up on. Slower, so a genuinely
// dead bus is not hammered - but still fast enough to catch a deliberate press,
// because the buttons are the only input the badge has and the bus being
// declared down is a heuristic, not a fact.
constexpr uint8_t BUTTON_POLL_DOWN_MS = 120;
// Held-key auto-repeat, used by menus and exposed to Lua as input.repeated().
constexpr uint16_t BUTTON_REPEAT_DELAY_MS  = 420;
constexpr uint16_t BUTTON_REPEAT_PERIOD_MS = 110;

// Expander bit index (P0..P5) for each logical key. Measured on a v1 badge by
// pressing each key and reading the line buttons.cpp logs per press - not
// assumed. The order is NOT the P0=up, P1=down, P2=left, P3=right that the
// firmware first guessed: down sits at the far end of the row, after left and
// right. That guess is what made two keys look dead. Physical DOWN drove P3 and
// so arrived as RIGHT, physical RIGHT drove P2 and arrived as LEFT, and
// left/right are unbound on the launcher - so both keys did nothing, while the
// two that landed on up/down still moved the cursor and looked fine.
constexpr uint8_t BTN_UP    = 0;  // P0, silkscreen UP
constexpr uint8_t BTN_LEFT  = 1;  // P1, silkscreen LEFT
constexpr uint8_t BTN_RIGHT = 2;  // P2, silkscreen RIGHT
constexpr uint8_t BTN_DOWN  = 3;  // P3, silkscreen DOWN
constexpr uint8_t BTN_A     = 4;  // P4, silkscreen SELECT
constexpr uint8_t BTN_B     = 5;  // P5, silkscreen CANCEL

// Holding B for this long inside a Lua app force-quits back to the launcher.
// An app that traps every button still cannot strand the user.
constexpr uint16_t APP_ESCAPE_HOLD_MS = 1500;

// ============================================================================
// RGB LEDs (2x WS2812B on a single bit-banged line)
// ============================================================================
constexpr uint8_t RGB_LED_COUNT       = 2;
constexpr uint8_t LED_DEFAULT_BRIGHTNESS = 72;   // WS2812 at full tilt is blinding

// ============================================================================
// Sensors
// ============================================================================
constexpr uint16_t PDM_SAMPLE_RATE      = 16000;
constexpr bool     MIC_SWAP_LR          = false;
constexpr float    BATTERY_DIVIDER_RATIO = 2.0f;  // 2k2 : 2k2
constexpr uint16_t BATTERY_POLL_MS      = 500;

// ============================================================================
// Networking
// ============================================================================
constexpr uint16_t PUSH_SERVER_PORT     = 80;
constexpr char     DEFAULT_HOSTNAME[]   = "solana-badge";
// SoftAP fallback, used by Settings -> Wi-Fi -> "Start hotspot".
constexpr char     DEFAULT_AP_PASSWORD[] = "solanabadge";
constexpr uint8_t  ESPNOW_DEFAULT_CHANNEL = 1;
constexpr uint16_t ESPNOW_BEACON_MS       = 1000;
// A peer that has not been heard from in this long drops off the radar.
constexpr uint32_t ESPNOW_PEER_TIMEOUT_MS = 12000;
constexpr uint8_t  ESPNOW_MAX_PEERS       = 20;
constexpr size_t   ESPNOW_MAX_PAYLOAD     = 240;  // ESP-NOW v1 frame limit is 250

// Largest single file the push endpoints will accept, in bytes. Bodies are
// buffered in RAM, so this is a heap guard, not a policy.
constexpr size_t   PUSH_MAX_FILE_BYTES = 96 * 1024;

// ============================================================================
// Identity (Ed25519, SE050-backed when the part cooperates)
// ============================================================================
// SE050 object id the identity keypair is created under. The 0xF0000000 range
// is the application space; anything below it belongs to NXP's own objects.
constexpr uint32_t SE050_IDENTITY_KEY_ID = 0xF0000001;
// Where the software fallback key and the badge's cached public key live. NVS
// keys are capped at 15 characters.
#define IDENTITY_NVS_NAMESPACE "badgeid"

// ============================================================================
// App-store broker - see broker/PROTOCOL.md
// ============================================================================
// Defaults to the official Solana DEF CON broker. Its Let's Encrypt trust
// anchor is compiled in (src/net/broker_ca.h), so https validates out of the
// box with no per-badge cert provisioning. Override it in Settings > App store
// or over the push API to point at another broker (upload a 'broker-ca' cert
// for a self-hosted https one).
#define DEFAULT_BROKER_URL "https://broker.solanadefcon.com"
// How often the badge asks whether anything is waiting for it. Short enough
// that "send to badge" feels immediate, long enough to be invisible on a
// conference network with a few hundred badges on it.
constexpr uint32_t BROKER_POLL_MS       = 6000;
// Backoff after a failed exchange, so a broker that is down or misaddressed
// costs one request a minute instead of one every six seconds.
constexpr uint32_t BROKER_RETRY_MS      = 60000;
// Deliberately short: this runs inside the main loop, and a stalled socket must
// not cost more than a couple of dropped frames.
constexpr uint16_t BROKER_HTTP_TIMEOUT_MS = 4000;
// Matches the broker's own per-offer cap.
constexpr uint8_t  BROKER_MAX_SCRIPTS   = 8;
constexpr size_t   BROKER_MAX_SCRIPT_BYTES = PUSH_MAX_FILE_BYTES;

// ============================================================================
// Lua runtime
// ============================================================================
// Hard ceiling on one app's Lua heap. The allocator draws from PSRAM and
// refuses past this, which surfaces to the app as a normal Lua "not enough
// memory" error rather than an OS-wide allocation failure.
constexpr size_t   LUA_HEAP_LIMIT_BYTES = 1024 * 1024;
// Wall-clock budget for a single Lua callback. Exceeding it raises an error in
// the app instead of hanging the badge; see runtime::armDeadline().
constexpr uint32_t LUA_CALLBACK_BUDGET_MS = 250;
constexpr uint32_t LUA_START_BUDGET_MS    = 5000;
// Ceiling on how much a callback can buy back with runtime::extendDeadline().
//
// Blocking bindings push the deadline out by what they intend to spend, which is
// right for one call and wrong for a loop of them: a binding that grants more
// milliseconds than it actually costs moves the deadline away faster than the
// clock reaches it, and the watchdog that exists to catch a runaway loop can
// never fire. badge.se050.random() grants 180 ms for an exchange that usually
// takes 30, so a loop around it freezes the badge outright rather than erroring.
// Capping the total per callback keeps the guarantee that a callback ends.
//
// Sized to clear the largest single legitimate wait - http.get at its 10 s
// timeout, plus its own slack - so one blocking call is never the thing that
// trips it.
constexpr uint32_t LUA_CALLBACK_EXTENSION_CAP_MS = 12000;
// Lua VM instructions between deadline checks. Small enough to catch a tight
// `while true do end`, large enough that the hook costs nothing measurable.
constexpr int      LUA_HOOK_INSTRUCTIONS  = 20000;

// Filesystem layout on the LittleFS partition.
#define FS_ROOT       "/littlefs"
#define APPS_DIR      "/apps"
#define LIB_DIR       "/lib"
#define CERTS_DIR     "/certs"
#define APP_MANIFEST  "app.ini"
#define APP_ENTRY     "main.lua"

// ============================================================================
// WPA2-Enterprise
//
// The badge has no RTC, so at boot the system clock reads 1970 and a
// certificate's notBefore is in the future - validation would fail on every
// otherwise-good certificate. Two ways out, and this picks the second:
//
//   1. set the clock before associating - impossible, that needs the network
//   2. seed the clock from the build timestamp and skip the validity-window
//      check, while still verifying the chain and the server's domain name
//
// EAP_DISABLE_TIME_CHECK controls (2). Turning it off means enterprise
// association only works on a badge whose clock has been set some other way.
constexpr bool EAP_DISABLE_TIME_CHECK = true;

// Defaults offered by the "DEF CON" preset in the web UI. The certificate is
// NOT bundled - it is reissued yearly, see src/net/cert_store.h.
#define DEFCON_SSID          "DefCon"
#define DEFCON_EAP_DOMAIN    "wifireg.defcon.org"
