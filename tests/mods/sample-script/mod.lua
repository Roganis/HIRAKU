-- A sample script (docs/MODS.md, "Scripts") for test_mod_script's made-up machine: the addresses
-- are made up. It keeps its numbers in Lua and writes them where the test looks (0x40 on), and
-- draws a cover over the picture's top left and a panel in the window's left bar.

local presses, releases, last_time, longest = 0, 0, 0, 0
local since = {}           -- a control's press time, while it's held
local early, late = 0, 0   -- judgments other than the best, early and late

-- [[hook]] at 0x1000: the time the press is judged by, earlier by the setting
function on_judge(regs, at)
  regs.r3 = regs.r3 - hk.setting("shift")
end

-- [[hook]] at 0x2000: r2 the judgment's kind (2 to 9 all but the best), r1 how far off (signed)
function on_result(regs, at)
  local kind, off = regs.r2, regs.r1
  if off >= 0x80000000 then off = off - 0x100000000 end
  if kind >= 2 and kind <= 9 then
    if off < 0 then early = early + 1 else late = late + 1 end
  end
end

function on_input(name, down, time)
  if down then
    presses = presses + 1
    since[name] = time
  else
    releases = releases + 1
    if since[name] then longest = math.max(longest, time - since[name]) end
    since[name] = nil
  end
  last_time = time
end

function on_frame()
  hk.write16(0x40, presses)
  hk.write16(0x42, releases)
  hk.write32(0x44, math.floor(last_time * 1000 + 0.5))
  hk.write16(0x48, early)
  hk.write16(0x4a, late)
  hk.write16(0x4c, math.floor(longest * 1000 + 0.5))
  hk.write8(0x4e, math.random(0, 255))   -- (math.random's sequence is the same each run)
  if hk.read8(0x30) == 1 then hk.set_setting("cover", 200) end
end

-- the game layer: the cover, a share of the picture's height (the setting, in 200ths) over its
-- left half; the presses under it
function draw_game(g)
  local h = hk.setting("cover") * g.height // 200
  g:rect(0, 0, g.width // 2, h, 0x404040)
  g:rect(g.width // 2, 0, 2, 2, hk.rgb(0, 0, 255, 128))
  g:text(0, g.height - 8, tostring(presses), 0xffffff)
end

-- the window layer: the left bar, where the picture leaves one, as a panel
function draw_window(w)
  if w.picture_x <= 0 then return end
  w:rect(0, 0, w.picture_x, w.height, hk.rgb(16, 20, 28))
  w:box(0, 0, w.picture_x, w.height, 0xff8000)
  w:line(1, w.height - 2, w.picture_x - 2, w.height - 2, 0x00ff00)
end
