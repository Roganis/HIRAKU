# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(hiraku LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# ccache, when it's installed: a file compiled before with the same flags comes from its cache
find_program(HK_CCACHE ccache)
if(HK_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_C_COMPILER_LAUNCHER ${HK_CCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${HK_CCACHE})
endif()

add_compile_options(-Wall -Wextra -fno-strict-aliasing)
# The same results on every host (the regress dumps are bit-exact): no fused multiply-adds,
# which compilers otherwise make wherever the host has them (every AArch64 CPU, x86-64 with
# -march), and a signed plain char, as on x86-64 (it is unsigned on AArch64).
add_compile_options(-ffp-contract=off -fsigned-char)
include_directories(${CMAKE_SOURCE_DIR}/include)
if(WIN32)
  # 64-bit file offsets (off_t, fseeko, stat) as on Linux: the disk images are bigger than 2 GiB
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()

# Profile-guided optimisation (tools/pgo.py): HIRAKU's machines are interpreters, schedulers and
# renderers whose hot paths clang lays out better when it has counts from real runs. pgo.py
# builds instrumented (HK_PGO_GENERATE), runs the regress cases it has dumps for, and writes one
# profile per core to HK_PGO_DIR. Per core, because the cores compile the same sources into
# separate copies (Musashi as a 68000 and as a 68EC020, MAME's shim for the XT446 and for the
# 573): one profile would blend their hot paths. Measured (PLAN.md section 14), and so applied:
# - firebeat: the runtime, the SPU's 68000, the XT446 (kbm -18 %, popn7 -3 %, bm3final -1 %); not
#   the recompiled game code (no gain, and only the trained games would have a profile);
# - sys573: MAME's code (-28 %);
# - DJ Main: none (5 % slower with its profile).
# The profiles stay out of the repo: they come from running the games.
# HK_GAME_BINARIES off: a release's build, with nothing in it that comes from a game (PLAN.md
# section 28): no Firebeat game binaries, no recorded 573/Twinkle blocks or XT446 MEG programs,
# only the cores' own binaries (hiraku-djmain, hiraku-sys573, hiraku-twinkle: --game ID)
option(HK_GAME_BINARIES "a binary per game (hiraku-<id>), with what build/ recorded for it" ON)
set(HK_PGO_DIR "" CACHE PATH "directory of per-core llvm-profdata profiles (tools/pgo.py)")
option(HK_PGO_GENERATE "build instrumented, to train the profiles (tools/pgo.py)" OFF)
set(HK_PGO_OK OFF)
if((HK_PGO_GENERATE OR HK_PGO_DIR) AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "PGO needs clang (tools/pgo.py configures it): building without")
elseif(HK_PGO_GENERATE)
  add_compile_options(-fprofile-instr-generate)
  add_link_options(-fprofile-instr-generate)
elseif(HK_PGO_DIR)
  set(HK_PGO_OK ON)
  message(STATUS "profile-guided: ${HK_PGO_DIR}")
endif()
# hk_pgo(core target...): those targets are optimised with the core's profile, when there is one
function(hk_pgo core)
  set(prof "${HK_PGO_DIR}/${core}.profdata")
  if(HK_PGO_OK AND EXISTS "${prof}")
    foreach(t ${ARGN})
      target_compile_options(${t} PRIVATE "-fprofile-instr-use=${prof}" -Wno-profile-instr-unprofiled
                             -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    endforeach()
  endif()
endfunction()

# --- decoder (shared) --------------------------------------------------------
add_library(hiraku_ppc STATIC src/ppc/decode.cpp src/ppc/analyze.cpp)
target_include_directories(hiraku_ppc PUBLIC src)

# --- the recompiler ----------------------------------------------------------
add_executable(hiraku-recomp recomp/main.cpp)
target_link_libraries(hiraku-recomp PRIVATE hiraku_ppc)

# --- Musashi 68000 core (MIT licence), configured as a plain 68000 ---------------
add_library(musashi STATIC
  third_party/musashi/m68kcpu.c
  third_party/musashi/m68kops.c
  third_party/musashi/m68kdasm.c
  third_party/musashi/softfloat/softfloat.c)
target_include_directories(musashi PUBLIC third_party/musashi runtime/spu)
target_compile_definitions(musashi PUBLIC MUSASHI_CNF="hk_m68kconf.h")
target_compile_options(musashi PRIVATE -w)

# ... and again as a 68EC020 with interrupt acknowledge (the DJ Main main CPU)
add_library(musashi_020 STATIC
  third_party/musashi/m68kcpu.c
  third_party/musashi/m68kops.c
  third_party/musashi/m68kdasm.c
  third_party/musashi/softfloat/softfloat.c)
target_include_directories(musashi_020 PUBLIC third_party/musashi runtime/djmain)
target_compile_definitions(musashi_020 PUBLIC MUSASHI_CNF="hk_m68kconf_dj.h")
target_compile_options(musashi_020 PRIVATE -w)

# --- Yamaha XT446 synth card (Keyboardmania): MAME's H8S/2655 and SWP30 (BSD-3) ----
# on a small framework shim (third_party/mame/README.md). h8make.py generates the
# H8 opcode dispatch (and disassembler tables) from h8.lst at build time.
set(HK_MAME ${CMAKE_SOURCE_DIR}/third_party/mame)
set(HK_MAME_GEN ${CMAKE_BINARY_DIR}/mame-gen)
set(HK_H8_GEN)
foreach(pair "o;h8" "h;h8h" "s20;h8s2000" "s26;h8s2600")
  list(GET pair 0 t)
  list(GET pair 1 n)
  foreach(mode s d)
    if(mode STREQUAL "s")
      set(out ${HK_MAME_GEN}/cpu/h8/${n}.hxx)
    else()
      set(out ${HK_MAME_GEN}/cpu/h8/${n}d.hxx)
    endif()
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${HK_MAME_GEN}/cpu/h8
      COMMAND python3 ${HK_MAME}/src/devices/cpu/h8/h8make.py ${HK_MAME}/src/devices/cpu/h8/h8.lst ${mode} ${t} ${out}
      DEPENDS ${HK_MAME}/src/devices/cpu/h8/h8make.py ${HK_MAME}/src/devices/cpu/h8/h8.lst
      COMMENT "h8make.py ${mode} ${t}")
    list(APPEND HK_H8_GEN ${out})
  endforeach()
endforeach()
set(HK_H8_SRC h8 h8h h8s2000 h8s2600 h8s2655 h8d h8hd h8s2000d h8s2600d
  h8_adc h8_dma h8_dtc h8_intc h8_port h8_sci h8_timer16 h8_timer8 h8_watchdog)
list(TRANSFORM HK_H8_SRC PREPEND ${HK_MAME}/src/devices/cpu/h8/)
list(TRANSFORM HK_H8_SRC APPEND .cpp)
add_library(hiraku_xt446 STATIC
  ${HK_MAME}/shim/emu.cpp
  ${HK_H8_SRC}
  ${HK_MAME}/src/devices/sound/swp30.cpp
  runtime/dev/xt446.cpp
  ${HK_H8_GEN})
target_include_directories(hiraku_xt446 PRIVATE ${HK_MAME}/shim ${HK_MAME}/src/devices ${HK_MAME_GEN})
target_include_directories(hiraku_xt446 PUBLIC runtime)
target_link_libraries(hiraku_xt446 PUBLIC hiraku_sys)
# The MEG's effect programs compiled to C++ from a recording (HK_MEG_RECORD=build/xt446/meg.txt,
# never committed: it holds the firmware's programs). Every build: meggen.py does nothing unless
# the output would change; with no recording the table is empty and the MEG is interpreted.
set(HK_MEG_GEN ${CMAKE_BINARY_DIR}/genmeg)
set(HK_MEG_RECORD ${CMAKE_SOURCE_DIR}/build/xt446/meg.txt)
if(NOT HK_GAME_BINARIES)
  set(HK_MEG_RECORD ${CMAKE_BINARY_DIR}/genmeg/no-recording.txt)   # never there: the MEG interpreted
endif()
add_custom_target(hiraku_meggen
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/meggen.py ${HK_MEG_RECORD} ${HK_MEG_GEN}
  BYPRODUCTS ${HK_MEG_GEN}/hk_meg_programs.inc)
add_dependencies(hiraku_xt446 hiraku_meggen)
target_include_directories(hiraku_xt446 PRIVATE ${HK_MEG_GEN})
# MAME sources: their own warning set; optimised even in debug builds (the card is real-time)
target_compile_options(hiraku_xt446 PRIVATE -w -O3)

add_executable(hiraku-xt446 tools/xt446/xt446_render.cpp runtime/core/sampler.cpp)
target_link_libraries(hiraku-xt446 PRIVATE hiraku_xt446)

# --- what Linux and Windows do differently (runtime/shell/sys.h) ------------------
add_library(hiraku_sys STATIC runtime/shell/sys.cpp runtime/shell/net.cpp runtime/shell/overlay.cpp)
target_include_directories(hiraku_sys PUBLIC runtime)
if(WIN32)
  target_link_libraries(hiraku_sys PUBLIC ws2_32)
endif()

# --- HIRAKU's e-AMUSEMENT server (docs/EAMUSE3.md): the Python games', the PC games' and
# hiraku-eamuse's (the Python core's own build compiles the same files, runtime/python/CMakeLists.txt)
# e-AMUSE3 itself (runtime/eamuse/: the calls, the cabinet's LAN, a real server online) and the
# System 573's GuitarFreaks and DrumMania answers
add_library(hiraku_eamuse STATIC runtime/eamuse/protocol.cpp runtime/eamuse/lan.cpp runtime/eamuse/server.cpp
            runtime/eamuse/remote.cpp runtime/sys573/eagfdm.cpp)
target_include_directories(hiraku_eamuse PUBLIC runtime)
find_package(Threads REQUIRED)
target_link_libraries(hiraku_eamuse PUBLIC hiraku_sys Threads::Threads)
add_library(hiraku_easerver STATIC runtime/python/easerver.cpp runtime/python/easerver_rank.cpp
            runtime/python/easerver_gfdm.cpp runtime/python/easerver_popn.cpp runtime/python/easerver_share.cpp
            runtime/python/easerver_pc.cpp runtime/python/easerver_hc.cpp runtime/python/easerver_p24.cpp runtime/python/easerver_iidx.cpp runtime/python/kbin.cpp runtime/python/lobby.cpp
            runtime/python/netrelay.cpp)
target_link_libraries(hiraku_easerver PUBLIC hiraku_eamuse)

# --- cabinet tables, lights file formats (shared with the launcher) -------------
add_library(hiraku_cabinet STATIC runtime/shell/cabinet.cpp runtime/host/hiddesc.cpp)
target_include_directories(hiraku_cabinet PUBLIC runtime)
target_link_libraries(hiraku_cabinet PUBLIC hiraku_sys)

# --- the shell: what every machine core shares (runtime/shell/core.h) --------------
# Input scripts; the host front end below. Cores provide hk::core().
add_library(hiraku_shell STATIC runtime/host/script.cpp runtime/host/macro.cpp runtime/host/mods.cpp runtime/host/pythondisk.cpp runtime/host/png.cpp
            runtime/host/acio.cpp runtime/host/readers.cpp runtime/host/bio2.cpp runtime/host/popnio.cpp)
target_include_directories(hiraku_shell PUBLIC runtime)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)   # mods' PNG pictures (runtime/host/png.h)
target_link_libraries(hiraku_shell PUBLIC hiraku_cabinet Threads::Threads ZLIB::ZLIB)
# The shell's main() (runtime/main.cpp) and the sampling profiler: compiled once for every core's
# binaries. Which game a per-game binary is: runtime/shell/built.h, a one-line file each
add_library(hiraku_main OBJECT runtime/main.cpp runtime/core/sampler.cpp)
target_compile_definitions(hiraku_main PRIVATE HK_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(hiraku_main PRIVATE hiraku_shell)
add_library(hiraku_built_none OBJECT runtime/shell/built_none.cpp)
# hiraku_built_game(<target> <id>): <target> is <id>'s binary (a core's own binary links
# hiraku_built_none instead)
function(hiraku_built_game target id)
  set(f ${CMAKE_BINARY_DIR}/built/${id}.cpp)
  file(CONFIGURE OUTPUT ${f} CONTENT "#include \"shell/built.h\"\nconst char *const hk::built_game = \"${id}\";\n")
  target_sources(${target} PRIVATE ${f})
endfunction()
# hiraku_game_link(<id> <core binary target>): a game with nothing of its own compiled in (no
# recorded code): hiraku-<id> is a link to its core's binary, which finds the game in its name.
# Windows (no links): a binary of its own, as before
function(hiraku_game_link id core)
  if(WIN32)
    add_executable(hiraku-${id} $<TARGET_OBJECTS:hiraku_main>)
    hiraku_built_game(hiraku-${id} ${id})
    get_target_property(libs ${core} LINK_LIBRARIES)
    list(REMOVE_ITEM libs hiraku_built_none)
    target_link_libraries(hiraku-${id} PRIVATE ${libs})
    return()
  endif()
  # a file, not a target: `ninja hiraku-<id>` makes it (tools/pgo.py), and it's renewed when the core is
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/hiraku-${id}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/hiraku-${id}
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:${core}> ${CMAKE_BINARY_DIR}/hiraku-${id}
    DEPENDS ${core} VERBATIM)
  set_property(GLOBAL APPEND PROPERTY HK_GAME_LINKS ${CMAKE_BINARY_DIR}/hiraku-${id})
endfunction()
# hiraku-ezusb.exe (Windows): the 32-bit go-between that loads pop'n music's ezusb.dll for the
# 64-bit games (runtime/host/popnio.h, ezusb/link.h). One C file, built by the same clang for i686
# beside the games (tools/package-win.sh takes every hiraku*.exe).
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/hiraku-ezusb.exe
    COMMAND ${CMAKE_C_COMPILER} --target=i686-w64-mingw32 -O2 -Wall -Wextra -fuse-ld=lld -static
            -o ${CMAKE_BINARY_DIR}/hiraku-ezusb.exe ${CMAKE_SOURCE_DIR}/runtime/host/ezusb/helper.c -lwinmm
    DEPENDS runtime/host/ezusb/helper.c runtime/host/ezusb/link.h
    COMMENT "hiraku-ezusb.exe (32-bit)")
  add_custom_target(hiraku-ezusb ALL DEPENDS ${CMAKE_BINARY_DIR}/hiraku-ezusb.exe)
  add_dependencies(hiraku_shell hiraku-ezusb)
endif()
# The PC games' cabinet (runtime/pc/, PLAN.md section 30): HIRAKU's ezusb.dll and libacio.dll, 32-bit
# like the games, which load them into their own process (under Wine on Linux). Built by clang for
# i686 Windows wherever it can: a Windows build, or Linux with mingw-w64's i686 libraries. Into pc/.
set(HK_PC_DLL_CC "")
set(HK_PC_DLL_SYS "")
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
  set(HK_PC_DLL_CC ${CMAKE_C_COMPILER})
elseif(NOT WIN32 AND EXISTS /usr/i686-w64-mingw32/lib/libkernel32.a)
  find_program(HK_CLANG clang)
  find_program(HK_LLD ld.lld)
  if(HK_CLANG AND HK_LLD)
    set(HK_PC_DLL_CC ${HK_CLANG})
    # mingw-w64's i686 headers and libraries named: clang finds them by its i686 gcc, which a
    # system with only the libraries (Ubuntu's mingw-w64-i686-dev, the release's) hasn't
    set(HK_PC_DLL_SYS -isystem /usr/i686-w64-mingw32/include -L/usr/i686-w64-mingw32/lib)
  endif()
endif()
if(HK_PC_DLL_CC)
  set(HK_PC_DLLS "")
  foreach(dll ezusb libacio)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pc/${dll}.dll
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pc
      COMMAND ${HK_PC_DLL_CC} --target=i686-w64-mingw32 ${HK_PC_DLL_SYS} -O2 -Wall -Wextra -fuse-ld=lld -static -shared
              -o ${CMAKE_BINARY_DIR}/pc/${dll}.dll ${CMAKE_SOURCE_DIR}/runtime/pc/${dll}.c
              ${CMAKE_SOURCE_DIR}/runtime/pc/cabinet.c ${CMAKE_SOURCE_DIR}/runtime/pc/dsound.c
              ${CMAKE_SOURCE_DIR}/runtime/pc/net.c ${CMAKE_SOURCE_DIR}/runtime/pc/vsock.c ${CMAKE_SOURCE_DIR}/runtime/pc/press.c
              ${CMAKE_SOURCE_DIR}/runtime/pc/d3d8.c
              ${CMAKE_SOURCE_DIR}/runtime/pc/${dll}.def
              -lpsapi -lgdi32 -ldxguid -lws2_32 -liphlpapi
      DEPENDS runtime/pc/${dll}.c runtime/pc/${dll}.def runtime/pc/cabinet.c runtime/pc/cabinet.h runtime/pc/link.h
              runtime/pc/dsound.c runtime/pc/net.c runtime/pc/vsock.c runtime/pc/press.c runtime/pc/d3d8.c
      COMMENT "pc/${dll}.dll (32-bit)")
    list(APPEND HK_PC_DLLS ${CMAKE_BINARY_DIR}/pc/${dll}.dll)
  endforeach()
  # The C02 cabinet's board (IIDX 9th style, PLAN.md section 33): an ezusb.dll of its own, in pc/c02/
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pc/c02/ezusb.dll
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pc/c02
    COMMAND ${HK_PC_DLL_CC} --target=i686-w64-mingw32 ${HK_PC_DLL_SYS} -O2 -Wall -Wextra -fuse-ld=lld -static -shared
            -o ${CMAKE_BINARY_DIR}/pc/c02/ezusb.dll ${CMAKE_SOURCE_DIR}/runtime/pc/ezusb_c02.c
            ${CMAKE_SOURCE_DIR}/runtime/pc/cabinet.c ${CMAKE_SOURCE_DIR}/runtime/pc/dsound.c
            ${CMAKE_SOURCE_DIR}/runtime/pc/net.c ${CMAKE_SOURCE_DIR}/runtime/pc/vsock.c ${CMAKE_SOURCE_DIR}/runtime/pc/press.c
            ${CMAKE_SOURCE_DIR}/runtime/pc/d3d8.c ${CMAKE_SOURCE_DIR}/runtime/pc/ezusb_c02.def
            -lpsapi -lgdi32 -ldxguid -lws2_32 -liphlpapi
    DEPENDS runtime/pc/ezusb_c02.c runtime/pc/ezusb_c02.def runtime/pc/cabinet.c runtime/pc/cabinet.h runtime/pc/link.h
            runtime/pc/dsound.c runtime/pc/net.c runtime/pc/vsock.c runtime/pc/press.c runtime/pc/d3d8.c
    COMMENT "pc/c02/ezusb.dll (32-bit)")
  list(APPEND HK_PC_DLLS ${CMAKE_BINARY_DIR}/pc/c02/ezusb.dll)
  add_custom_target(hiraku-pc-dlls ALL DEPENDS ${HK_PC_DLLS})
  # vsock.c (NET TAISEN over the relay) as the games call it: tests/pc/test_vsock.sh runs it under Wine
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pc/test_vsock.exe
    COMMAND ${HK_PC_DLL_CC} --target=i686-w64-mingw32 ${HK_PC_DLL_SYS} -O2 -Wall -Wextra -fuse-ld=lld -static
            -o ${CMAKE_BINARY_DIR}/pc/test_vsock.exe ${CMAKE_SOURCE_DIR}/tests/pc/test_vsock.c
            ${CMAKE_SOURCE_DIR}/runtime/pc/vsock.c -lws2_32
    DEPENDS tests/pc/test_vsock.c runtime/pc/vsock.c runtime/pc/cabinet.h
    COMMENT "pc/test_vsock.exe (32-bit)")
  add_custom_target(hiraku-pc-test-vsock ALL DEPENDS ${CMAKE_BINARY_DIR}/pc/test_vsock.exe)
  # The 32-bit AVS2 PC games (runtime/pc32/, PLAN.md section 35): hiraku-pc32.exe, the loader that
  # starts Konami's AVS2 and the game's DLL, with the DLLs above as its cabinet. Into pc/.
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pc/hiraku-pc32.exe
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pc
    COMMAND ${HK_PC_DLL_CC} --target=i686-w64-mingw32 ${HK_PC_DLL_SYS} -O2 -Wall -Wextra -fuse-ld=lld -static
            -o ${CMAKE_BINARY_DIR}/pc/hiraku-pc32.exe ${CMAKE_SOURCE_DIR}/runtime/pc32/main.c ${CMAKE_SOURCE_DIR}/runtime/pc32/files.c
            ${CMAKE_SOURCE_DIR}/runtime/pc32/songs.c ${CMAKE_SOURCE_DIR}/runtime/pc32/tables.c ${CMAKE_SOURCE_DIR}/runtime/pc32/x86.c
            -lole32 -lwindowscodecs -luuid ${CMAKE_SOURCE_DIR}/runtime/pc64/imports.c
    DEPENDS runtime/pc32/main.c runtime/pc32/files.c runtime/pc32/songs.c runtime/pc32/tables.c runtime/pc32/x86.c runtime/pc32/x86.h
            runtime/pc32/pc32.h runtime/pc64/imports.c runtime/pc64/pc64.h
    COMMENT "pc/hiraku-pc32.exe (32-bit)")
  add_custom_target(hiraku-pc32 ALL DEPENDS ${CMAKE_BINARY_DIR}/pc/hiraku-pc32.exe)
else()
  message(STATUS "No 32-bit Windows compiler (clang, lld and mingw-w64's i686 libraries): no PC games' DLLs")
endif()
# The 64-bit PC pop'n (runtime/pc64/, PLAN.md section 32): hiraku-pc64.exe, the loader that starts
# Konami's AVS2 and the game's DLL with the cabinet around them, 64-bit like them. Into pc/.
set(HK_PC64_CC "")
set(HK_PC64_SYS "")
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(HK_PC64_CC ${CMAKE_C_COMPILER})
elseif(NOT WIN32 AND EXISTS /usr/x86_64-w64-mingw32/lib/libkernel32.a)
  find_program(HK_CLANG clang)
  find_program(HK_LLD ld.lld)
  if(HK_CLANG AND HK_LLD)
    set(HK_PC64_CC ${HK_CLANG})
    set(HK_PC64_SYS -isystem /usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib)
  endif()
endif()
if(HK_PC64_CC)
  set(HK_PC64_SOURCES main.c log.c imports.c files.c link.c display.c board.c net.c touch.c sound.c script.c crt.c songs.c)
  list(TRANSFORM HK_PC64_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/runtime/pc64/ OUTPUT_VARIABLE HK_PC64_PATHS)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pc/hiraku-pc64.exe
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pc
    COMMAND ${HK_PC64_CC} --target=x86_64-w64-mingw32 ${HK_PC64_SYS} -O2 -Wall -Wextra -fuse-ld=lld -static
            -o ${CMAKE_BINARY_DIR}/pc/hiraku-pc64.exe ${HK_PC64_PATHS}
            -ld3d9 -lsetupapi -lws2_32 -liphlpapi -lole32 -luuid
    DEPENDS ${HK_PC64_PATHS} runtime/pc64/pc64.h runtime/pc/link.h
    COMMENT "pc/hiraku-pc64.exe (64-bit)")
  add_custom_target(hiraku-pc64 ALL DEPENDS ${CMAKE_BINARY_DIR}/pc/hiraku-pc64.exe)
else()
  message(STATUS "No 64-bit Windows compiler (clang, lld and mingw-w64's x86_64 libraries): no hiraku-pc64")
endif()
# NFC card readers through PC/SC (runtime/host/readers.h, --scard): optional
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PCSC QUIET IMPORTED_TARGET libpcsclite)
endif()
# HK_PCSC_DLOPEN (Linux; a release's): libpcsclite loaded when first used, not linked
# (runtime/host/pcsc_load.cpp), so the binaries start without it; only its headers are needed
option(HK_PCSC_DLOPEN "Linux: load libpcsclite at run time instead of linking it" OFF)
if(NOT WIN32 AND HK_PCSC_DLOPEN)
  find_path(HK_PCSC_INCLUDE PCSC/winscard.h)
endif()
if(WIN32)
  target_link_libraries(hiraku_shell PRIVATE winscard)   # Windows' own PC/SC
  target_compile_definitions(hiraku_shell PRIVATE HK_WITH_PCSC=1)
elseif(HK_PCSC_DLOPEN AND HK_PCSC_INCLUDE)
  target_sources(hiraku_shell PRIVATE runtime/host/pcsc_load.cpp)
  target_include_directories(hiraku_shell PRIVATE ${HK_PCSC_INCLUDE} ${HK_PCSC_INCLUDE}/PCSC)
  target_link_libraries(hiraku_shell PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(hiraku_shell PRIVATE HK_WITH_PCSC=1)
  message(STATUS "hiraku: PC/SC loaded at run time: NFC card readers where libpcsclite is installed")
elseif(PCSC_FOUND)
  target_link_libraries(hiraku_shell PRIVATE PkgConfig::PCSC)
  target_compile_definitions(hiraku_shell PRIVATE HK_WITH_PCSC=1)
  message(STATUS "hiraku: PC/SC ${PCSC_VERSION}: NFC card readers enabled")
endif()

# ATA/ATAPI bus and disc/disk images (both cores)
# Disc and disk images, CHDs read in place (runtime/dev/image.h): libchdr (BSD-3), its FLAC
# decoder (dr_flac, public domain) and the LZMA SDK's decoder (public domain), in third_party/libchdr
# zstd through pkg-config where there is one; a portable build (tools/package.sh) turns
# pkg-config off, and finds the library and its header itself
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
endif()
if(TARGET PkgConfig::ZSTD)
  set(HK_ZSTD PkgConfig::ZSTD)
else()
  find_path(HK_ZSTD_INCLUDE zstd.h REQUIRED)
  find_library(HK_ZSTD_LIBRARY zstd REQUIRED)
  add_library(hk_zstd INTERFACE)
  target_include_directories(hk_zstd INTERFACE ${HK_ZSTD_INCLUDE})
  target_link_libraries(hk_zstd INTERFACE ${HK_ZSTD_LIBRARY})
  set(HK_ZSTD hk_zstd)
endif()
find_package(ZLIB REQUIRED)
add_library(hk_libchdr STATIC
  third_party/libchdr/src/libchdr_bitstream.c third_party/libchdr/src/libchdr_cdrom.c
  third_party/libchdr/src/libchdr_chd.c third_party/libchdr/src/libchdr_flac.c
  third_party/libchdr/src/libchdr_huffman.c third_party/libchdr/lzma/LzmaDec.c)
target_include_directories(hk_libchdr PUBLIC third_party/libchdr/include PRIVATE third_party/libchdr/lzma)
target_compile_definitions(hk_libchdr PRIVATE _7ZIP_ST Z7_ST)
target_compile_options(hk_libchdr PRIVATE -O2 -w)
target_link_libraries(hk_libchdr PUBLIC ${HK_ZSTD} ZLIB::ZLIB)
# libchdr_mini.so (Windows: libchdr_mini.dll): libchdr as a shared library for tools/chdread.py
# (Python ingest reads a Python game's disk through it); a package carries it in build/, where
# chdread.py looks
add_library(chdr_mini SHARED
  third_party/libchdr/src/libchdr_bitstream.c third_party/libchdr/src/libchdr_cdrom.c
  third_party/libchdr/src/libchdr_chd.c third_party/libchdr/src/libchdr_flac.c
  third_party/libchdr/src/libchdr_huffman.c third_party/libchdr/lzma/LzmaDec.c)
target_include_directories(chdr_mini PRIVATE third_party/libchdr/include third_party/libchdr/lzma)
target_compile_definitions(chdr_mini PRIVATE _7ZIP_ST Z7_ST)
target_compile_options(chdr_mini PRIVATE -O2 -w)
target_link_libraries(chdr_mini PRIVATE ${HK_ZSTD} ZLIB::ZLIB)
add_library(hiraku_image STATIC runtime/dev/image.cpp)
target_include_directories(hiraku_image PUBLIC runtime)
target_link_libraries(hiraku_image PUBLIC hk_libchdr hiraku_sys)
add_executable(hiraku-chdcat runtime/dev/chdcat.cpp)
target_link_libraries(hiraku-chdcat PRIVATE hiraku_image)

add_library(hiraku_ata STATIC runtime/dev/ata.cpp)
target_include_directories(hiraku_ata PUBLIC runtime)
target_link_libraries(hiraku_ata PUBLIC hiraku_image)

# --- the Firebeat core: PPC403 runtime and the Firebeat board ------------------------
add_library(hiraku_rt STATIC
  runtime/core/state.cpp
  runtime/core/interp.cpp
  runtime/core/dispatch.cpp
  runtime/core/patched.cpp
  runtime/core/sched.cpp
  runtime/core/cpu403.cpp
  runtime/core/glue.cpp
  runtime/core/exec.cpp
  runtime/dev/k057714.cpp
  runtime/dev/rtc65271.cpp
  runtime/dev/flash29f016a.cpp
  runtime/dev/fdc.cpp
  runtime/dev/rf5c400.cpp
  runtime/dev/ymz280b.cpp
  runtime/spu/spu.cpp
  runtime/firebeat/board.cpp
  runtime/firebeat/spectrum.cpp
  runtime/firebeat/synth.cpp)
target_include_directories(hiraku_rt PUBLIC runtime)
target_link_libraries(hiraku_rt PUBLIC hiraku_ppc musashi hiraku_ata hiraku_cabinet hiraku_xt446)
hk_pgo(firebeat hiraku_rt musashi hiraku_xt446)   # not the game code: see hk_pgo above

# --- the DJ Main core: an interpreted 68EC020 and the DJ Main board -----------------
add_library(hiraku_djmain STATIC
  runtime/djmain/board.cpp
  runtime/djmain/k056832.cpp
  runtime/djmain/k054539.cpp
  runtime/djmain/core.cpp)
target_include_directories(hiraku_djmain PUBLIC runtime)
target_link_libraries(hiraku_djmain PUBLIC musashi_020 hiraku_ata hiraku_cabinet)
target_compile_options(hiraku_djmain PRIVATE -O2)

# Keyboardmania's synth stand-in (optional: FluidSynth, LGPL, linked dynamically)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FLUIDSYNTH QUIET IMPORTED_TARGET fluidsynth)
endif()
if(FLUIDSYNTH_FOUND)
  target_link_libraries(hiraku_rt PUBLIC PkgConfig::FLUIDSYNTH)
  target_compile_definitions(hiraku_rt PRIVATE HK_WITH_FLUIDSYNTH=1)
  message(STATUS "hiraku: FluidSynth ${FLUIDSYNTH_VERSION}: Keyboardmania synth stand-in enabled")
endif()

# --- host front end (optional: SDL3) ---------------------------------------------
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  # HID outputs (controller LEDs), shared with the launcher
  add_library(hiraku_hidout STATIC runtime/host/hidout.cpp)
  target_link_libraries(hiraku_hidout PUBLIC SDL3::SDL3 hiraku_cabinet)
  add_library(hiraku_host STATIC runtime/host/window.cpp runtime/host/alsa.cpp runtime/host/notice.cpp runtime/host/lampview.cpp runtime/host/shaderview.cpp runtime/host/picture.cpp runtime/host/input.cpp runtime/host/lights.cpp
              runtime/host/pacing.cpp)
  target_include_directories(hiraku_host PUBLIC runtime)
  target_link_libraries(hiraku_host PUBLIC SDL3::SDL3 hiraku_shell hiraku_hidout)
  target_compile_definitions(hiraku_host PUBLIC HK_WITH_SDL=1)
  target_link_libraries(hiraku_main PRIVATE hiraku_host)
  if(WIN32)   # cardio readers through SDL's HID calls (readers.cpp)
    target_link_libraries(hiraku_shell PUBLIC SDL3::SDL3)
    target_compile_definitions(hiraku_shell PRIVATE HK_WITH_SDL=1)
  endif()
  if(WIN32)   # WASAPI exclusive mode and ASIO (--audio-exclusive, --audio-asio: runtime/host/wasapi.h, asio.h)
    target_sources(hiraku_host PRIVATE runtime/host/wasapi.cpp runtime/host/asio.cpp runtime/host/dwm.cpp)
    target_link_libraries(hiraku_host PRIVATE ole32 avrt uuid)
  endif()
  # Wayland: the compositor's presentation times and fifo, for display pacing (runtime/host/pacing.h).
  # Found without pkg-config (the laptop package turns it off); libwayland-client is on any system
  # with Mesa's EGL.
  find_path(WAYLAND_CLIENT_INCLUDE wayland-client.h)
  find_library(WAYLAND_CLIENT_LIB wayland-client)
  if(WAYLAND_CLIENT_INCLUDE AND WAYLAND_CLIENT_LIB)
    target_sources(hiraku_host PRIVATE third_party/wayland/presentation-time-protocol.c
                                       third_party/wayland/fifo-v1-protocol.c
                                       third_party/wayland/content-type-v1-protocol.c)
    target_include_directories(hiraku_host PRIVATE third_party/wayland ${WAYLAND_CLIENT_INCLUDE})
    target_link_libraries(hiraku_host PRIVATE ${WAYLAND_CLIENT_LIB})
    target_compile_definitions(hiraku_host PRIVATE HK_WITH_WAYLAND=1)
    message(STATUS "hiraku: Wayland presentation feedback and fifo enabled")
  endif()
  if(WIN32)   # SDL3.dll beside the games, where Windows looks for it
    add_custom_target(hiraku_sdl3_dll ALL
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> ${CMAKE_BINARY_DIR})
  endif()
  message(STATUS "hiraku: SDL3 front end enabled")
  # hiraku-pc: the PC games, Konami's own programs, as a core of HIRAKU's shell (runtime/pc/core.cpp,
  # PLAN.md section 30); its DLLs are the hiraku-pc-dlls above
  add_executable(hiraku-pc runtime/pc/core.cpp)
  target_link_libraries(hiraku-pc PRIVATE hiraku_main hiraku_built_none hiraku_easerver hiraku_host hiraku_shell)
  target_compile_definitions(hiraku-pc PRIVATE HK_ROOT="${CMAKE_SOURCE_DIR}")
  if(TARGET hiraku-pc-dlls)
    add_dependencies(hiraku-pc hiraku-pc-dlls)
  endif()
endif()

# --- the launcher (optional: Qt 6 Widgets + SDL3) ------------------------------------
find_package(Qt6 QUIET COMPONENTS Widgets Network)
if(Qt6_FOUND AND TARGET hiraku_hidout)
  set(CMAKE_AUTOMOC ON)
  add_executable(hiraku
    launcher/main.cpp
    launcher/aboutdialog.cpp
    launcher/config.cpp
    launcher/games.cpp
    launcher/inputs.cpp
    launcher/mainwindow.cpp
    launcher/capturedialog.cpp
    launcher/controlsdialog.cpp
    launcher/lightsdialog.cpp
    launcher/dipsdialog.cpp
    launcher/modsdialog.cpp
    launcher/settingsdialog.cpp
    launcher/fit.cpp
    launcher/picturedialog.cpp
    launcher/unlocksdialog.cpp
    launcher/updater.cpp)
  qt_add_resources(hiraku "launcher_res" PREFIX "/" BASE launcher/res
    FILES launcher/res/icon.png launcher/res/logo.png)
  target_compile_definitions(hiraku PRIVATE HK_ROOT="${CMAKE_SOURCE_DIR}")
  target_link_libraries(hiraku PRIVATE Qt6::Widgets Qt6::Network hiraku_cabinet hiraku_hidout)
  set(CMAKE_AUTOMOC OFF)
  message(STATUS "hiraku: Qt ${Qt6_VERSION} launcher enabled")
endif()

# --- tests -------------------------------------------------------------------
# HK_TESTS off for a release's package (tools/release.sh; on when HK_TESTS=ON is in its environment)
option(HK_TESTS "the unit tests (tests/)" ON)
if(HK_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- games -------------------------------------------------------------------
# A game is built when analyze/prep.py has produced build/<id>/recomp.txt
# (which needs the user's own dump: nothing game-derived is in the repo).
set(HK_RECOMP_FILES 32)
# hiraku_game(<id> <entry> [<load address, default 0x80000000>])
function(hiraku_game id entry)
  set(load 0x80000000)
  if(ARGC GREATER 2)
    set(load ${ARGV2})
  endif()
  set(spec ${CMAKE_SOURCE_DIR}/build/${id}/recomp.txt)
  if(NOT HK_GAME_BINARIES)   # a release: no game's code in it (PLAN.md section 27)
    return()
  endif()
  if(NOT EXISTS ${spec})
    message(STATUS "hiraku: ${id}: no ${spec}, skipped (run tools/ingest.py, analyze/ghidra.sh, analyze/prep.py)")
    return()
  endif()
  set(gen ${CMAKE_BINARY_DIR}/gen/${id})
  set(outs ${gen}/funcs.h ${gen}/table.c)
  math(EXPR last "${HK_RECOMP_FILES} - 1")
  foreach(k RANGE ${last})
    string(LENGTH "${k}" len)
    if(len EQUAL 1)
      set(k "00${k}")
    elseif(len EQUAL 2)
      set(k "0${k}")
    endif()
    list(APPEND outs ${gen}/code_${k}.c)
  endforeach()
  add_custom_command(
    OUTPUT ${outs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen}
    COMMAND hiraku-recomp ${spec} ${gen} --files ${HK_RECOMP_FILES}
    DEPENDS hiraku-recomp ${spec}
    COMMENT "Recompiling ${id}")
  add_library(hiraku_${id}_code STATIC ${outs})
  target_include_directories(hiraku_${id}_code PRIVATE ${gen})
  # generated code: unused labels and parameters are normal
  target_compile_options(hiraku_${id}_code PRIVATE -w -O2 -fwrapv)
  add_executable(hiraku-${id} runtime/firebeat/core.cpp)
  target_compile_definitions(hiraku-${id} PRIVATE
    HK_GAME_ID="${id}" HK_ENTRY=${entry}u HK_LOAD=${load}u HK_ROOT="${CMAKE_SOURCE_DIR}")
  hiraku_built_game(hiraku-${id} ${id})
  if(TARGET hiraku_host)
    target_link_libraries(hiraku-${id} PRIVATE hiraku_host)
  endif()
  target_link_libraries(hiraku-${id} PRIVATE hiraku_main hiraku_shell hiraku_rt hiraku_${id}_code hiraku_rt)
endfunction()

# --- the System 573 core (PLAN.md section 19): MAME's PS1 and System 573 devices
# (BSD-3) on the framework shim, run by MAME's own scheduler (exact mode) --------
set(HK_573_MAME
  shim/emu.cpp shim/screen.cpp shim/machine/ram.cpp shim/imagedev/cdromimg.cpp shim/stubs/ata_stubs.cpp
  src/emu/dirtc.cpp
  src/devices/cpu/psx/psx.cpp src/devices/cpu/psx/gte.cpp src/devices/cpu/psx/dma.cpp src/devices/cpu/psx/irq.cpp
  src/devices/cpu/psx/rcnt.cpp src/devices/cpu/psx/sio.cpp src/devices/cpu/psx/mdec.cpp src/devices/cpu/psx/psxdasm.cpp
  src/devices/video/psx.cpp
  src/devices/sound/spu.cpp src/devices/sound/spureverb.cpp src/devices/sound/spu_tables.cpp src/devices/sound/cdda.cpp
  src/devices/sound/mas3507d.cpp src/devices/sound/mp3_audio.cpp
  src/devices/machine/intelfsh.cpp src/devices/machine/timekpr.cpp src/devices/machine/adc083x.cpp
  src/devices/machine/ds2401.cpp src/devices/machine/x76f041.cpp src/devices/machine/x76f100.cpp
  src/devices/machine/atahle.cpp src/devices/machine/t10mmc.cpp src/devices/machine/t10spc.cpp
  src/devices/bus/ata/ataintf.cpp src/devices/bus/ata/atadev.cpp src/devices/bus/ata/atapihle.cpp
  src/devices/bus/ata/atapicdr.cpp src/devices/bus/ata/cr589.cpp
  src/devices/bus/pccard/pccard.cpp src/devices/bus/pccard/linflash.cpp
  src/devices/machine/jvshost.cpp src/devices/machine/jvsdev.cpp src/devices/bus/psx/ctlrport.cpp src/devices/bus/psx/memcard.cpp
  src/mame/konami/k573cass.cpp src/mame/konami/zs01.cpp src/mame/konami/k573dio.cpp src/mame/konami/k573fpga.cpp src/mame/konami/k573mcr.cpp)
list(APPEND HK_573_MAME
  # the multi-session unit's TX3927 CPU (mips1), and the serial parts between it and the 573
  src/devices/cpu/mips/mips1.cpp src/devices/cpu/mips/mips1dsm.cpp src/emu/diserial.cpp src/devices/machine/ins8250.cpp
  src/devices/sound/tc9446f.cpp src/mame/konami/k573msu.cpp src/devices/cpu/psx/sio_uart.cpp
  # MAME's ATA hard disk (an option of every ATA slot; Twinkle's SPU board has one), on a raw image
  src/devices/bus/ata/hdd.cpp src/devices/machine/atastorage.cpp shim/imagedev/harddriv.cpp)
list(TRANSFORM HK_573_MAME PREPEND ${HK_MAME}/)
# softfloat3 (Berkeley SoftFloat 3, BSD-3): the MIPS CPUs' FPU in mips1; MAME builds it as C++
file(GLOB_RECURSE HK_SF3_SRC ${HK_MAME}/src/3rdparty/softfloat3/source/*.c)
set_source_files_properties(${HK_SF3_SRC} PROPERTIES LANGUAGE CXX)
add_library(hk_softfloat3 STATIC ${HK_SF3_SRC})
target_include_directories(hk_softfloat3 PRIVATE ${HK_MAME}/src/3rdparty/softfloat3/build/MAME
  ${HK_MAME}/src/3rdparty/softfloat3/source ${HK_MAME}/src/3rdparty/softfloat3/source/8086)
target_include_directories(hk_softfloat3 PUBLIC ${HK_MAME}/src/3rdparty/softfloat3/source/include)
target_compile_definitions(hk_softfloat3 PUBLIC SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16
  SOFTFLOAT_FAST_DIV64TO32 SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1)
target_compile_options(hk_softfloat3 PRIVATE -w -O2)
set(HK_573_INC ${HK_MAME}/shim ${HK_MAME}/shim/stubs ${HK_MAME}/src/devices ${HK_MAME}/src/mame/konami
  ${HK_MAME}/src/emu ${HK_MAME}/src/3rdparty)
# The System 573's MAME code (CPU interpreter, GPU, SPU): mostly MAME's interpreter, which
# PGO (above) speeds up by ~30 % (PLAN.md section 19, 573-4). Safe with -march: contraction
# into fused multiply-adds is off for everything (above).
set(HK_573_OPT "-O3" CACHE STRING "compiler optimisation flags for the System 573's MAME code")
separate_arguments(HK_573_OPT_LIST UNIX_COMMAND "${HK_573_OPT}")
add_library(hiraku_mame573 STATIC ${HK_573_MAME})
target_include_directories(hiraku_mame573 PUBLIC ${HK_573_INC})
target_link_libraries(hiraku_mame573 PUBLIC hiraku_image)   # CHDs read in place (runtime/dev/image.h)
target_compile_options(hiraku_mame573 PRIVATE -w ${HK_573_OPT_LIST})
target_link_libraries(hiraku_mame573 PUBLIC hk_softfloat3)

add_library(hiraku_sys573 STATIC runtime/sys573/board.cpp runtime/sys573/core.cpp runtime/sys573/mips.cpp runtime/sys573/npu.cpp runtime/sys573/link.cpp
  runtime/sys573/cardunit.cpp runtime/sys573/rental.cpp runtime/sys573/mcal.cpp)
target_include_directories(hiraku_sys573 PUBLIC runtime)
target_link_libraries(hiraku_sys573 PUBLIC hiraku_mame573 hiraku_cabinet hiraku_eamuse)
# hiraku-eamuse: the e-AMUSEMENT server on the network, for several cabinets (tools/eamuse/server.cpp):
# the System 573's and the Python core's (docs/EAMUSE3.md); plain C++, no emulator
add_executable(hiraku-eamuse tools/eamuse/server.cpp)
target_include_directories(hiraku-eamuse PRIVATE runtime)
target_link_libraries(hiraku-eamuse PRIVATE hiraku_easerver)
target_compile_options(hiraku_sys573 PRIVATE -O2 -Wno-unused-parameter)
hk_pgo(sys573 hiraku_mame573)

# hiraku_sys573_game(<id>): a System 573 game. MAME's devices, and the game's recompiled R3000
# blocks when there is a recording of them, build/<id>/mips573.txt (runtime/sys573/mips.h,
# tools/mipsrec.py): tools/mipsgen.py turns it into C++. Without one, the CPU is all interpreted.
set(HK_573_BLOCK_FILES 8)
set(HK_573_MIN_RUNS 1 CACHE STRING "blocks run fewer times than this in the recording are left to the interpreter")
function(hiraku_sys573_game id)
  set(rec ${CMAKE_SOURCE_DIR}/build/${id}/mips573.txt)
  if(NOT ARGV1 STREQUAL "CORE" AND NOT EXISTS ${rec})   # nothing recorded: the core's binary
    hiraku_game_link(${id} hiraku-sys573)
    return()
  endif()
  set(gen ${CMAKE_BINARY_DIR}/gen573/${id})
  set(outs ${gen}/table.cpp)
  math(EXPR last "${HK_573_BLOCK_FILES} - 1")
  foreach(k RANGE ${last})
    list(APPEND outs ${gen}/blocks_${k}.cpp)
  endforeach()
  # every build: mipsgen.py does nothing unless the recording (or the script) changed, and
  # writes empty tables while there is none, so a new recording needs no cmake run
  add_custom_target(hiraku_${id}_mipsgen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/mipsgen.py ${rec} ${gen} --files ${HK_573_BLOCK_FILES} --min-runs ${HK_573_MIN_RUNS}
    BYPRODUCTS ${outs}
    COMMENT "Recompiling ${id}'s R3000 blocks, if its recording changed")
  add_library(hiraku_${id}_blocks STATIC ${outs})
  add_dependencies(hiraku_${id}_blocks hiraku_${id}_mipsgen)
  target_link_libraries(hiraku_${id}_blocks PRIVATE hiraku_mame573)
  target_include_directories(hiraku_${id}_blocks PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
  # generated code: unused variables and labels are normal; wrapping arithmetic is the guest's
  target_compile_options(hiraku_${id}_blocks PRIVATE -w -O2 -fwrapv)
  add_executable(hiraku-${id} $<TARGET_OBJECTS:hiraku_main>)
  if(ARGV1 STREQUAL "CORE")
    target_link_libraries(hiraku-${id} PRIVATE hiraku_built_none)
  else()
    hiraku_built_game(hiraku-${id} ${id})
  endif()
  if(TARGET hiraku_host)
    target_link_libraries(hiraku-${id} PRIVATE hiraku_host)
  endif()
  target_link_libraries(hiraku-${id} PRIVATE hiraku_shell hiraku_sys573 hiraku_${id}_blocks)
endfunction()

# --- the Twinkle core (PLAN.md section 21): beatmania IIDX. The 573's PS1 devices (above), plus the
# SPU board's 68000 (MAME's generated core: m68000-sdf/sdp are ~4 MB each), SCSI, the super I/O,
# the dongles and the DVD player's serial port -------------------------------------------------------
set(HK_TW_MAME
  src/devices/cpu/m68000/m68kcommon.cpp src/devices/cpu/m68000/m68000.cpp src/devices/cpu/m68000/m68000-decode.cpp
  src/devices/cpu/m68000/m68000-sdf.cpp src/devices/cpu/m68000/m68000-sdp.cpp src/devices/cpu/m68000/m68kdasm.cpp
  src/devices/machine/ncr53c90.cpp src/devices/machine/nscsi_bus.cpp src/devices/machine/nscsi_hle.cpp
  src/devices/bus/nscsi/cd.cpp src/devices/machine/fdc37c665gt.cpp src/devices/machine/i2cmem.cpp
  src/devices/machine/rtc65271.cpp src/devices/machine/watchdog.cpp src/devices/machine/mb8421.cpp
  src/devices/sound/rf5c400.cpp src/devices/bus/rs232/rs232.cpp src/devices/bus/rs232/xvd701.cpp
  shim/stubs/twinkle_stubs.cpp)
list(TRANSFORM HK_TW_MAME PREPEND ${HK_MAME}/)
add_library(hiraku_mametw STATIC ${HK_TW_MAME})
target_link_libraries(hiraku_mametw PUBLIC hiraku_mame573)
target_compile_options(hiraku_mametw PRIVATE -w -O2)

# the PS1's R3000 recompiled as on the 573 (runtime/sys573/mips.h: the same code, built in here too)
# and the 573's session cable (runtime/sys573/link.h), which joins a club kit's IIDX cabinet to its DDR one
add_library(hiraku_twinkle STATIC runtime/twinkle/board.cpp runtime/twinkle/core.cpp runtime/sys573/mips.cpp
  runtime/sys573/link.cpp)
target_include_directories(hiraku_twinkle PUBLIC runtime)
# the background video (runtime/twinkle/video.h): the Video CD decoded with FFmpeg's libraries,
# when there are any (without them, Twinkle runs with MAME's picture)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(HK_FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
endif()
if(HK_FFMPEG_FOUND)
  target_sources(hiraku_twinkle PRIVATE runtime/twinkle/video.cpp)
  target_compile_definitions(hiraku_twinkle PRIVATE HK_TWINKLE_VIDEO=1)
  target_link_libraries(hiraku_twinkle PUBLIC PkgConfig::HK_FFMPEG)
else()
  message(STATUS "hiraku: no FFmpeg libraries (libavformat, libavcodec): Twinkle without its background video")
endif()
target_link_libraries(hiraku_twinkle PUBLIC hiraku_mametw hiraku_cabinet)
target_compile_options(hiraku_twinkle PRIVATE -O2 -Wno-unused-parameter)
# Twinkle's own MAME devices (the 68000, SCSI, the RF5C400...) with Twinkle's profile; the PS1 parts
# it shares with the 573 (hiraku_mame573) keep the 573's
hk_pgo(twinkle hiraku_mametw hiraku_twinkle)

# hiraku_twinkle_game(<id>): a Twinkle game. MAME's devices; the game's R3000 blocks recompiled from
# a recording, build/<id>/mips573.txt, as hiraku_sys573_game does (tools/mipsrec.py); the 68000
# interpreted. The data comes from dumps/<id>/ at run time
function(hiraku_twinkle_game id)
  set(rec ${CMAKE_SOURCE_DIR}/build/${id}/mips573.txt)
  if(NOT ARGV1 STREQUAL "CORE" AND NOT EXISTS ${rec})   # nothing recorded: the core's binary
    hiraku_game_link(${id} hiraku-twinkle)
    return()
  endif()
  set(gen ${CMAKE_BINARY_DIR}/gen573/${id})
  set(outs ${gen}/table.cpp)
  math(EXPR last "${HK_573_BLOCK_FILES} - 1")
  foreach(k RANGE ${last})
    list(APPEND outs ${gen}/blocks_${k}.cpp)
  endforeach()
  add_custom_target(hiraku_${id}_mipsgen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/mipsgen.py ${rec} ${gen} --files ${HK_573_BLOCK_FILES} --min-runs ${HK_573_MIN_RUNS}
    BYPRODUCTS ${outs}
    COMMENT "Recompiling ${id}'s R3000 blocks, if its recording changed")
  add_library(hiraku_${id}_blocks STATIC ${outs})
  add_dependencies(hiraku_${id}_blocks hiraku_${id}_mipsgen)
  target_link_libraries(hiraku_${id}_blocks PRIVATE hiraku_mame573)
  target_include_directories(hiraku_${id}_blocks PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
  target_compile_options(hiraku_${id}_blocks PRIVATE -w -O2 -fwrapv)
  add_executable(hiraku-${id} $<TARGET_OBJECTS:hiraku_main>)
  if(ARGV1 STREQUAL "CORE")
    target_link_libraries(hiraku-${id} PRIVATE hiraku_built_none)
  else()
    hiraku_built_game(hiraku-${id} ${id})
  endif()
  if(TARGET hiraku_host)
    target_link_libraries(hiraku-${id} PRIVATE hiraku_host)
  endif()
  target_link_libraries(hiraku-${id} PRIVATE hiraku_shell hiraku_twinkle hiraku_${id}_blocks)
endfunction()

# hiraku_djmain_game(<id>): a DJ Main game. No generated code: the program is
# interpreted, so every game's binary is hiraku-djmain's (a link to it; the data comes from
# dumps/<id>/ at run time, written by tools/ingest.py).
function(hiraku_djmain_game id)
  if(NOT ARGV1 STREQUAL "CORE")
    hiraku_game_link(${id} hiraku-djmain)
    return()
  endif()
  add_executable(hiraku-${id} $<TARGET_OBJECTS:hiraku_main>)
  target_link_libraries(hiraku-${id} PRIVATE hiraku_built_none)
  if(TARGET hiraku_host)
    target_link_libraries(hiraku-${id} PRIVATE hiraku_host)
  endif()
  target_link_libraries(hiraku-${id} PRIVATE hiraku_shell hiraku_djmain)
endfunction()

# The cores' own binaries, hiraku-djmain, hiraku-sys573 and hiraku-twinkle: any of the core's games,
# told with --game ID (runtime/main.cpp), with no recorded blocks (the CPU interpreted). A release has
# only these (HK_GAME_BINARIES off): nothing in them comes from a game.
hiraku_djmain_game(djmain CORE)
hiraku_sys573_game(sys573 CORE)
hiraku_twinkle_game(twinkle CORE)

# every games/<id>/game.toml with core = "djmain" (MAME's DJ Main sets), "sys573", "twinkle": a
# binary per game (hiraku-<id>), with the game's recorded blocks when build/<id>/ has them
file(GLOB HK_GAME_TOMLS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/*/game.toml)
if(NOT HK_GAME_BINARIES)
  set(HK_GAME_TOMLS "")
endif()
foreach(toml ${HK_GAME_TOMLS})
  file(STRINGS ${toml} djmain_core REGEX "^core *= *\"djmain\"")
  if(djmain_core)
    get_filename_component(dir ${toml} DIRECTORY)
    get_filename_component(id ${dir} NAME)
    hiraku_djmain_game(${id})
  endif()
  file(STRINGS ${toml} sys573_core REGEX "^core *= *\"sys573\"")
  if(sys573_core)
    get_filename_component(dir ${toml} DIRECTORY)
    get_filename_component(id ${dir} NAME)
    hiraku_sys573_game(${id})
  endif()
  file(STRINGS ${toml} twinkle_core REGEX "^core *= *\"twinkle\"")
  if(twinkle_core)
    get_filename_component(dir ${toml} DIRECTORY)
    get_filename_component(id ${dir} NAME)
    hiraku_twinkle_game(${id})
  endif()
endforeach()

# the games' links (hiraku_game_link), made with the cores' binaries; a new recording
# (build/<id>/mips573.txt) makes its game a binary of its own at the next build
get_property(HK_GAME_LINKS GLOBAL PROPERTY HK_GAME_LINKS)
if(HK_GAME_LINKS)
  add_custom_target(hiraku_game_links ALL DEPENDS ${HK_GAME_LINKS})
endif()
file(GLOB HK_573_RECORDINGS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/build/*/mips573.txt)

# hiraku-firebeat: every Firebeat game, from its program and recomp.txt, run by the executor
# (runtime/core/exec.h: no compiler, no game's code in the binary; PLAN.md section 27)
add_executable(hiraku-firebeat runtime/firebeat/core.cpp runtime/core/no_table.c)
target_compile_definitions(hiraku-firebeat PRIVATE HK_EXECUTOR=1 HK_ROOT="${CMAKE_SOURCE_DIR}")
if(TARGET hiraku_host)
  target_link_libraries(hiraku-firebeat PRIVATE hiraku_host)
endif()
target_link_libraries(hiraku-firebeat PRIVATE hiraku_main hiraku_built_none hiraku_shell hiraku_rt)

hiraku_game(bm3final 0x80003000)
hiraku_game(bm3 0x80003000)
hiraku_game(bm3core 0x80003000)
hiraku_game(bm36th 0x80003000)
hiraku_game(bm37th 0x80003000)
hiraku_game(popn4 0x80003000)
hiraku_game(popn5 0x80003000)
hiraku_game(popn6 0x80002018)
hiraku_game(popn7 0x80002018)
hiraku_game(ppp1mp 0x80002018)
hiraku_game(ppp 0x80002018)
hiraku_game(ppp11 0x80002018)
hiraku_game(ppd 0x80002018)
hiraku_game(popnanm 0x80003000)
hiraku_game(popnanm2 0x80003000)
hiraku_game(popn8 0x80002018)
hiraku_game(popnmt2 0x80002018)
hiraku_game(kbm 0x80083000 0x80080000)
hiraku_game(kbh 0x80083000 0x80080000)
hiraku_game(kbm2nd 0x80002018)
hiraku_game(kbm3rd 0x80002018)
