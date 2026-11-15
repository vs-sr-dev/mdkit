-- MAME autoboot script for tools/mamecmp.py: the snapshots and the pad hand md.exe gives.
--
--   MDSNAP   frames to snapshot (md.exe's numbering, from 0), comma-separated; exits after the last
--   MDSEED   the monkey's seed (md.exe --monkey SEED): the same hand, frame for frame
--   MDPRESS  or a script: "FRAME+LEN:button;..." (md.exe --press)
--
-- md.exe sets the pad before running frame f; here it is set at the end of the frame before.
local snaps, last = {}, 0
for f in string.gmatch(os.getenv("MDSNAP") or "300", "%d+") do
  f = tonumber(f); snaps[f] = true; if f > last then last = f end
end
local seed = tonumber(os.getenv("MDSEED") or "")
local presses = {}
for a, l, name in string.gmatch(os.getenv("MDPRESS") or "", "(%d+)%+(%d+):([^;]+)") do
  presses[#presses + 1] = {from = tonumber(a), to = tonumber(a) + tonumber(l), name = name}
end

local M = 0xFFFFFFFF
local function monkey(frame)
  local x = ((seed * 2654435761) & M) ~ (((frame // 6) * 40503) & M) ~ 0x9E3779B9
  for _ = 1, 4 do
    x = (x ~ (x << 13)) & M
    x = x ~ (x >> 17)
    x = (x ~ (x << 5)) & M
  end
  local b = {}
  local f = frame % 600
  b.start = (frame > 90 and f < 4) or (frame < 400 and frame % 120 < 3)
  b.a = (x & 1) ~= 0
  b.b = (x & 2) ~= 0
  b.c = (x & 12) == 12
  local dirs = {0, 1, 2, 4, 8, 8, 5, 9}
  local d = dirs[((x >> 4) & 7) + 1]
  b.up, b.down, b.left, b.right = (d & 1) ~= 0, (d & 2) ~= 0, (d & 4) ~= 0, (d & 8) ~= 0
  return b
end

local names = {up = "P1 Up", down = "P1 Down", left = "P1 Left", right = "P1 Right",
               a = "P1 A", b = "P1 B", c = "P1 C", start = "P1 Start",
               x = "P1 X", y = "P1 Y", z = "P1 Z", mode = "P1 Mode"}
local fields = {}
for tag, port in pairs(manager.machine.ioport.ports) do
  if string.find(tag, ":ctrl1:", 1, true) then
    for name, fld in pairs(port.fields) do fields[name] = fld end
  end
end

local scr = manager.machine.screens:at(1)
local n = -1        -- the md.exe frame just ended
emu.register_frame_done(function()
  n = n + 1
  local nxt = n + 1
  local held = {}
  if seed then held = monkey(nxt) end
  for _, p in ipairs(presses) do
    if nxt >= p.from and nxt < p.to then held[p.name] = true end
  end
  for k, fname in pairs(names) do
    local fld = fields[fname]
    if fld then fld:set_value(held[k] and 1 or 0) end
  end
  if snaps[n] then scr:snapshot(string.format("f%05d.png", n)) end
  if n >= last then manager.machine:exit() end
end)
