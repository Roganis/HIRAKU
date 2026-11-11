-- SPDX-License-Identifier: GPL-3.0-or-later
-- beatmania III's floppy controller (FDC37C665GT at 0x70001000, ISA port = offset / 4): every
-- access, a line each, in <run>/fdc.txt: "seconds frame R|W port value". The runtime writes the
-- same file with HK_FDC_LOG=FILE; oracle/fdc.py lists and compares them.
--
--   HIRAKU_LUA=oracle/lua/fdc.lua MAME_ARGS="-flop DISK.img" oracle/mame.sh bm3final SCRIPT.his
--
-- The first line is the board's clock as the machine starts ("rtc YYYY-MM-DD HH:MM:SS", read from
-- the RTC65271's registers: MAME sets it from the host's time when it starts the machine, a
-- moment before this script runs): the games put the time in what they write to a disk, so the
-- runtime is given the same (HK_RTC).
local out = assert(os.getenv("HIRAKU_OUT"), "HIRAKU_OUT not set")
local f = assert(io.open(out .. "/fdc.txt", "w"))
local machine = manager.machine
local space = machine.devices[":maincpu"].spaces["program"]
local screen = machine.screens[":screen"]
do
    -- (the chip's registers are read through its index register: written here before the BIOS runs)
    local function reg(n)
        space:write_u8(0x7e000000, n)
        return space:read_u8(0x7e000001)
    end
    local binary = reg(0x0b) & 0x04 ~= 0
    local function num(n)
        local v = reg(n)
        if n == 4 then v = v & 0x7f end   -- (the hours' PM bit: the games run the clock on 24 hours)
        return binary and v or (v >> 4) * 10 + (v & 15)
    end
    f:write(string.format("rtc %04d-%02d-%02d %02d:%02d:%02d\n", 2000 + num(9), num(8), num(7), num(4), num(2), num(0)))
    f:write("# the host: " .. os.date("%Y-%m-%d %H:%M:%S") .. "\n")
end
local function line(kind, offset, data, mask)
    if mask & 0xff000000 == 0 then return end
    f:write(string.format("%.6f f%d %s %x %02x\n", machine.time:as_double(), screen:frame_number() + 1, kind,
        0x3f0 + (((offset & 0xfff) >> 2) & 7), (data >> 24) & 0xff))
end
-- (globals: a tap that is collected stops firing)
hiraku_fdc_r = space:install_read_tap(0x70001000, 0x70001fff, "hiraku_fdc_r", function(offset, data, mask) line("R", offset, data, mask) end)
hiraku_fdc_w = space:install_write_tap(0x70001000, 0x70001fff, "hiraku_fdc_w", function(offset, data, mask) line("W", offset, data, mask) end)
hiraku_fdc_stop = emu.add_machine_stop_notifier(function() f:close() end)
