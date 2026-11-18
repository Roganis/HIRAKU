# Lua

The interpreter of mods' scripts (docs/MODS.md, "Scripts"; `runtime/host/modscript.cpp`).

`src/`: Lua 5.4.9 (released 2026-08-10), the C files and headers of the release's `src/` folder as
they are, less `lua.c` and `luac.c` (the stand-alone interpreter and compiler) and the Makefile, from
https://www.lua.org/ftp/lua-5.4.9.tar.gz (373429 bytes, SHA-256
`2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`, as https://www.lua.org/ftp/ gives
it). Nothing in them is changed: what HIRAKU needs is set at compile time (CMakeLists.txt, `hk_lua`):
`luai_makeseed` a fixed number, so that a run's string hashes, and so the order `pairs` gives, are
the same each time.

`LICENSE`: Lua's licence (MIT), the notice at the end of `src/lua.h`. Copyright (C) 1994-2026
Lua.org, PUC-Rio.
