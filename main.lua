-- Eject Seato
--
-- Turns the badge into its own Wi-Fi hotspot and serves one static page on
-- it. The actual HTTP server is the OS's push_server - the main loop already
-- brings that up on port 80 the moment wifi.hotspot() makes the badge look
-- "connected" (AccessPoint mode counts). This app just points wifi.serve_page
-- at the page it wants shown at "/" instead of the normal push UI, and clears
-- that override again on exit so the push UI comes back for anything that
-- follows.
--
-- The hotspot always has a WPA2 password - there is no open-AP mode in the
-- SDK - so a fixed password is set here rather than assumed, and it is the
-- one shown on screen.

local g = badge.gfx

local SSID = badge.device_name
local PASSWORD = "ejectoseato"
local PAGE_HTML = "<p>ejecto seato</p>"

local status = "starting..."
local ap_ok = false
local served = false

function on_start()
  badge.led.take()

  ap_ok = badge.wifi.hotspot(PASSWORD)
  if ap_ok then
    badge.wifi.serve_page(PAGE_HTML)
    served = true
    status = "up"
    badge.log("eject: hotspot '" .. SSID .. "' up, serving / on port 80")
  else
    status = "hotspot failed to start"
    badge.log("eject: " .. status)
  end
end

function on_update(dt)
  if ap_ok then
    badge.led.gradient(0, 0.5, 0.5)
    badge.led.gradient(1, 0.5, 0.5)
  else
    badge.led.all(255, 0, 0)
  end
  badge.led.show()
end

function on_draw()
  g.clear(g.BG)

  g.text_center("Eject Seato", g.width() // 2, 40, g.WHITE, 2)

  if not ap_ok then
    g.text_center(status, g.width() // 2, 100, g.MUTED, 1)
    g.text_center("CANCEL to quit", g.width() // 2, g.height() - 14, g.MUTED, 1)
    return
  end

  local ip = badge.wifi.ip()
  local cy = 80

  g.text_center("network", g.width() // 2, cy, g.MUTED, 1)
  g.text_center(SSID, g.width() // 2, cy + 16, g.SOLANA_GREEN, 1)

  g.text_center("password", g.width() // 2, cy + 40, g.MUTED, 1)
  g.text_center(PASSWORD, g.width() // 2, cy + 56, g.WHITE, 1)

  g.text_center("open http://" .. ip .. "/", g.width() // 2, cy + 80, g.SOLANA_PURPLE, 1)

  g.text_center("CANCEL to quit", g.width() // 2, g.height() - 14, g.MUTED, 1)
end

function on_button(key, pressed)
  if not pressed then return end
  if key == "b" then
    badge.system.exit()
  end
end

function on_stop()
  if served then
    badge.wifi.clear_page()
  end
  if ap_ok then
    badge.wifi.disconnect()
  end
  badge.led.off()
end
