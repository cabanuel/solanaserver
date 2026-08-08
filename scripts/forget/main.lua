-- Forget About It
--
-- Plays brian-oconner-roman-pearce.gif on the panel. Built off apps/gallery -
-- same "measure once, cache it, only decode when the picture actually changes"
-- shape - but a GIF instead of stills means two adjustments:
--
-- gfx.image() only speaks PNG and baseline JPEG (src/lua_sdk/lib_gfx.cpp), and
-- the source GIF is 2.4 MB against the SDK's 256 KB per-file decode limit
-- anyway. So the GIF stays in this directory purely as the source asset, and
-- tools/make-frames.py explodes it into frame-NN.jpg (one baseline JPEG per
-- frame, scaled down) plus durations.lua (the per-frame delay, pulled from the
-- GIF itself so a re-timed source regenerates a re-timed app). Re-run it after
-- replacing the GIF, then push the whole directory:
--
--   python3 apps/forget/tools/make-frames.py
--   tools/badge-push.py --host <ip> --token <code> push apps/forget
--
-- And unlike gallery's arbitrary pushed pictures, every frame here shares one
-- size by construction, so the fit-to-screen scale only needs computing once
-- - on the first frame - rather than per file on every page turn.

local g = badge.gfx

local W, H = g.width(), g.height()
local BAR_H = 36                 -- caption strip along the bottom
local AREA_H = H - BAR_H         -- everything above it belongs to the picture

local frames = {}       -- sorted frame-NN.jpg names in this app's directory
local durations = {}    -- seconds per frame, same length as frames, from the GIF
local index = 1         -- 1-based selection into frames
local playing = true
local elapsed = 0       -- seconds spent on the current frame

local draw_x, draw_y, draw_scale = 0, 0, 1   -- computed once, shared by every frame
local dirty = true

-- ---------------------------------------------------------------------------
-- Discovery

local function is_frame(name)
  return name:sub(1, 6) == "frame-" and name:sub(-4) == ".jpg"
end

local function load_frames()
  local names = badge.storage.list()
  if not names then return end

  for _, name in ipairs(names) do
    if is_frame(name) then frames[#frames + 1] = name end
  end
  table.sort(frames)

  local ok, loaded = pcall(require, "durations")
  if ok and type(loaded) == "table" then durations = loaded end

  badge.log("forget: " .. #frames .. " frame(s)")
end

-- Every frame was produced at the same size, so this only has to run once - on
-- whatever ends up in frames[1] - rather than being re-measured on every
-- advance the way gallery re-measures per file.
local function measure()
  local name = frames[1]
  if not name then return end

  local w, h = g.image_size(name)
  if not (w and h and w > 0 and h > 0) then return end

  local fit = math.min(W / w, AREA_H / h)
  draw_scale = math.max(0.05, math.min(8, fit))
  draw_x = math.floor((W - w * draw_scale) / 2)
  draw_y = math.floor((AREA_H - h * draw_scale) / 2)
end

-- ---------------------------------------------------------------------------
-- Drawing

local function ellipsise(text, limit)
  if g.text_width(text) <= limit then return text end
  while #text > 1 and g.text_width(text .. "..") > limit do
    text = text:sub(1, #text - 1)
  end
  return text .. ".."
end

local function draw_error(name)
  local w, h = 268, 92
  local x, y = (W - w) // 2, (AREA_H - h) // 2

  g.clear(g.BG)
  g.fill_round_rect(x, y, w, h, 6, g.PANEL)
  g.round_rect(x, y, w, h, 6, g.RED)
  g.text_center("CANNOT DECODE", W // 2, y + 16, g.RED, 1)
  g.text_center(ellipsise(name, w - 24), W // 2, y + 40, g.WHITE, 1)
  g.text_center("re-run tools/make-frames.py", W // 2, y + 60, g.MUTED, 1)
end

local function draw_missing()
  g.text_center("NO FRAMES", W // 2, 44, g.WHITE, 2)
  g.text_center("run tools/make-frames.py and push this app", W // 2, 74, g.MUTED, 1)
end

local function draw_caption()
  g.fill_rect(0, AREA_H, W, BAR_H, g.PANEL)
  g.line(0, AREA_H, W - 1, AREA_H, g.BORDER)

  local state = playing and "PLAYING" or "PAUSED"
  local counter = (#frames == 0 and 0 or index) .. " / " .. #frames
  g.text(state .. "  " .. counter, 10, AREA_H + 8, g.SOLANA_GREEN, 1)

  g.text("LEFT/RIGHT step   SELECT " .. (playing and "pause" or "play") .. "   CANCEL quit",
         10, AREA_H + 22, g.MUTED, 1)
end

-- ---------------------------------------------------------------------------
-- Lifecycle

function on_start()
  badge.led.take()
  load_frames()
  measure()
end

local function step(delta)
  if #frames == 0 then return end
  index = (index - 1 + delta) % #frames + 1
  elapsed = 0
  playing = false
  dirty = true
end

function on_update(dt)
  if not playing or #frames == 0 then return end

  elapsed = elapsed + dt
  local hold = durations[index] or 0.1
  if elapsed >= hold then
    elapsed = elapsed - hold
    index = index % #frames + 1
    dirty = true
  end
end

function on_draw()
  if not dirty then return end
  dirty = false

  g.clear(g.BG)

  local name = frames[index]
  if name == nil then
    draw_missing()
  else
    local drew, reason = g.image(name, draw_x, draw_y, draw_scale)
    if not drew then
      badge.log("forget: " .. name .. ": " .. tostring(reason))
      draw_error(name)
    end
  end
  draw_caption()

  local t = 0
  if #frames > 1 then t = (index - 1) / (#frames - 1) end
  badge.led.gradient(0, t, 0.6)
  badge.led.gradient(1, t, 0.6)
  badge.led.show()
end

function on_button(key, pressed)
  if not pressed then return end

  if key == "left" or key == "up" then
    step(-1)
  elseif key == "right" or key == "down" then
    step(1)
  elseif key == "a" then
    playing = not playing
    elapsed = 0
    dirty = true
  elseif key == "b" then
    badge.system.exit()
  end
end

function on_stop()
  badge.led.off()
end
