# Lua (third_party/lua): the interpreter of mods' scripts (runtime/host/modscript.h), listed once for
# HIRAKU's build and the Python core's (runtime/python/CMakeLists.txt). hk_add_lua(<target>) makes
# the static library <target> of it.
#
# Only what a script may use is compiled: the core, the auxiliary library and the base, string,
# table, math and utf8 libraries (no io, os, package, debug or coroutine: their files are in
# third_party/lua/src as released, but not built). The C files are compiled as C++, so that an error
# in a script unwinds HIRAKU's own functions it passed through (Lua throws instead of longjmp), and
# with luai_makeseed fixed: a script's string hashes, and so the order `pairs` gives, are the same
# from run to run.
set(HK_LUA_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/lua/src)
set(HK_LUA_SOURCES)
foreach(f lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring
          ltable ltm lundump lvm lzio lauxlib lbaselib lstrlib ltablib lmathlib lutf8lib)
  list(APPEND HK_LUA_SOURCES ${HK_LUA_DIR}/${f}.c)
endforeach()

function(hk_add_lua target)
  add_library(${target} STATIC ${HK_LUA_SOURCES})
  set_source_files_properties(${HK_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
  target_include_directories(${target} PUBLIC ${HK_LUA_DIR})
  # (an option, not a definition: CMake drops a definition with parentheses; exceptions on where a
  # build turns them off, as PCSX2's does)
  if(MSVC)
    target_compile_options(${target} PRIVATE /w /EHsc "/Dluai_makeseed(L)=0x484b4d53u")
  else()
    target_compile_options(${target} PRIVATE -w -fexceptions "-Dluai_makeseed(L)=0x484b4d53u")
  endif()
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
