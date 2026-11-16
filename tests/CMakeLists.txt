# SPDX-License-Identifier: GPL-3.0-or-later

# A Windows build made on Linux: ctest starts the tests' programs under Wine, where there is one
# (the release workflow's Windows job does; by hand: WINEPREFIX=... WINEDEBUG=-all ctest, with no
# DISPLAY for nothing to show). The Python tests are the build machine's own: those that start one
# of the programs are left to the Linux build.
if(WIN32 AND CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  find_program(HK_WINE wine)
  if(HK_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${HK_WINE})
  endif()
endif()

# Instruction semantics vs QEMU. The reference cases are generated, not
# committed: tests/ppc/gen.py needs clang, ld.lld and qemu-ppc (no test without them).
find_program(HK_QEMU_PPC qemu-ppc)
find_program(HK_LD_LLD ld.lld)
if(HK_QEMU_PPC AND HK_LD_LLD)
add_executable(test_ppc ppc/test_ppc.cpp)
target_link_libraries(test_ppc PRIVATE hiraku_rt)

set(HK_PPC_CASES ${CMAKE_BINARY_DIR}/tests/ppc-cases)
add_custom_command(
  OUTPUT ${HK_PPC_CASES}/cases.bin ${HK_PPC_CASES}/names.txt
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/ppc/gen.py ${HK_PPC_CASES}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ppc/gen.py
  COMMENT "Generating PowerPC reference cases with qemu-ppc")
add_custom_target(ppc_cases DEPENDS ${HK_PPC_CASES}/cases.bin)
add_dependencies(test_ppc ppc_cases)
add_test(NAME ppc_semantics COMMAND test_ppc ${HK_PPC_CASES})
endif()
# hiraku-recomp: a nopoll function still asks at its entry whether a mod changed it, and files that
# didn't change aren't written again (a made-up program)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(TARGET Python3::Interpreter)
  set_property(TARGET Python3::Interpreter PROPERTY CROSSCOMPILING_EMULATOR "")   # (the build machine's Python)
endif()
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME recomp_nopoll COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/ppc/test_recomp_nopoll.py
           $<TARGET_FILE:hiraku-recomp> ${CMAKE_C_COMPILER} ${CMAKE_SOURCE_DIR}/include)
endif()

# GCU renderer vs MAME: replay a MAME trace's GCU writes (oracle, not a ctest).
add_executable(gcu_replay gcu/gcu_replay.cpp)
target_link_libraries(gcu_replay PRIVATE hiraku_rt)

# HID output descriptors and reports (controller lights)
add_executable(test_hiddesc hid/test_hiddesc.cpp)
target_link_libraries(test_hiddesc PRIVATE hiraku_cabinet)
add_test(NAME hid_outputs COMMAND test_hiddesc)

# Konami ACIO devices on the PC (runtime/host/acio.h). Real card readers: the serial reader's dialogue against a pretend reader on a pseudo-terminal
# (Linux only: Windows has no pseudo-terminals to pretend with)
if(NOT WIN32)
  add_executable(test_readers acio/test_readers.cpp)
  target_link_libraries(test_readers PRIVATE hiraku_shell)
  add_test(NAME card_readers COMMAND test_readers)
  add_test(NAME card_readers_one_bus COMMAND test_readers 2)   # a cabinet's two readers on one port
  # beatmania IIDX's BIO2 I/O board against a pretend BI2A and a pretend machine
  add_executable(test_bio2 acio/test_bio2.cpp)
  target_link_libraries(test_bio2 PRIVATE hiraku_shell)
  add_test(NAME bio2_board COMMAND test_bio2)
endif()

# A pop'n music cabinet's USB I/O board (runtime/host/popnio.h) against a pretend board and a pretend machine
add_executable(test_popnio popnio/test_popnio.cpp)
target_link_libraries(test_popnio PRIVATE hiraku_shell)
add_test(NAME popn_io_board COMMAND test_popnio)
# Windows: the same through hiraku-ezusb.exe and a pretend 32-bit ezusb.dll (under Wine here); all
# three beside each other, where HIRAKU looks for the helper
if(WIN32 AND TARGET hiraku-ezusb)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/fake_ezusb.dll
    COMMAND ${CMAKE_C_COMPILER} --target=i686-w64-mingw32 -O2 -shared -fuse-ld=lld -static
            -o ${CMAKE_BINARY_DIR}/fake_ezusb.dll ${CMAKE_CURRENT_SOURCE_DIR}/popnio/fake_ezusb.c
            ${CMAKE_CURRENT_SOURCE_DIR}/popnio/fake_ezusb.def
    DEPENDS popnio/fake_ezusb.c popnio/fake_ezusb.def)
  add_custom_target(fake_ezusb DEPENDS ${CMAKE_BINARY_DIR}/fake_ezusb.dll)
  add_executable(test_ezusb_helper popnio/test_ezusb_helper.cpp)
  target_link_libraries(test_ezusb_helper PRIVATE hiraku_shell)
  set_target_properties(test_ezusb_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(test_ezusb_helper fake_ezusb hiraku-ezusb)
  add_test(NAME popn_io_ezusb_helper COMMAND test_ezusb_helper ${CMAKE_BINARY_DIR}/fake_ezusb.dll)
endif()

# Windows: the DXGI swap chain's queue of a frame's other pictures (runtime/host/swapchain.h: smooth
# motion's in-between ones, presented by a thread of its own at their times) and display pacing's
# accounting of them, run as the window runs them against a pretend Direct3D device, swap chain,
# display and OpenGL (under Wine here: no graphics card needed); and the old way, each picture
# waited for on the game's thread
if(WIN32 AND TARGET hiraku_host)
  add_executable(test_swapchain swapchain/test_swapchain.cpp swapchain/fake_d3d.c
                 ${CMAKE_SOURCE_DIR}/runtime/host/swapchain.cpp ${CMAKE_SOURCE_DIR}/runtime/host/pacing.cpp)
  target_include_directories(test_swapchain PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
  target_link_libraries(test_swapchain PRIVATE SDL3::SDL3)
  set_target_properties(test_swapchain PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})   # (beside SDL3.dll)
  add_test(NAME swapchain_queue COMMAND test_swapchain queue)
  add_test(NAME swapchain_wait COMMAND test_swapchain wait)
  # ... and, at the game's own speed, a 58 Hz game on a 120 Hz display: the frames' work (the controls
  # read, the sound made) at the game's even pace, their pictures on the display's refreshes
  add_test(NAME swapchain_even COMMAND test_swapchain even 120 1.0)
  # ... the frames' own pictures presented by the game's thread (HK_DXGI_QUEUE_OWN=0), and the
  # cabinet's tight case: a frame's work, a refresh and its pictures' drawing more than a frame
  add_test(NAME swapchain_own COMMAND test_swapchain own)
  add_test(NAME swapchain_tight COMMAND test_swapchain tight 120 5.0 900)
endif()

# beatmania III's floppy controller and drive (runtime/dev/fdc.h) against a made-up disk: an empty
# drive as before, a cylinder read, written and read back, disk change, write protect
add_executable(test_fdc fdc/test_fdc.cpp)
target_link_libraries(test_fdc PRIVATE hiraku_rt)
add_test(NAME floppy_controller COMMAND test_fdc)

# ATA security (SECURITY UNLOCK) on a hard disk, as MAME does it (DJ Main complete MIX on)
add_executable(test_ata_security ata/test_ata_security.cpp)
target_link_libraries(test_ata_security PRIVATE hiraku_ata)
target_include_directories(test_ata_security PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME ata_security COMMAND test_ata_security)
# ... and the bus's state across commands a host cuts short (IDENTIFY after an untaken read, a soft
# reset mid-read, the empty unit chosen while data is going out)
add_executable(test_ata_state ata/test_ata_state.cpp)
target_link_libraries(test_ata_state PRIVATE hiraku_ata)
target_include_directories(test_ata_state PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME ata_state COMMAND test_ata_state)

# The picture's shader engine: FrameCount, frame history, feedback (needs a display and OpenGL
# 3.3; skipped without them)
if(TARGET hiraku_host)
  add_executable(test_picture picture/test_picture.cpp)
  target_link_libraries(test_picture PRIVATE hiraku_host)
  add_test(NAME picture_engine COMMAND test_picture)
  set_tests_properties(picture_engine PROPERTIES SKIP_RETURN_CODE 77)
  if(HK_SANITIZE)   # (SDL and the graphics driver keep a few hundred bytes to the end: not HIRAKU's to free)
    set_tests_properties(picture_engine PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()

# Host controls (runtime/host/input.h) with a controller SDL makes up: bindings that share an input,
# a controller unplugged and plugged back in
if(TARGET hiraku_host)
  add_executable(test_bindings input/test_bindings.cpp)
  target_link_libraries(test_bindings PRIVATE hiraku_host)
  add_test(NAME input_bindings COMMAND test_bindings)
endif()

# The launcher's MIDI player's file reader (runtime/midi/smf.h): made-up files of each kind, cut
# ones, and starting part-way
add_executable(test_smf midi/test_smf.cpp)
target_link_libraries(test_smf PRIVATE hiraku_midi)
add_test(NAME midi_files COMMAND test_smf)
# ... and its volume for a synth that plays on its own (Windows'): in each part's volume controller
add_executable(test_part_volume midi/test_part_volume.cpp)
target_include_directories(test_part_volume PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME midi_part_volume COMMAND test_part_volume)

# The performance overlay (runtime/host/statsview.h): what it reads of the system's files (made-up
# ones), its panel from made-up frames, its levels, and its place: the corners, a drag with the
# mouse, kept in a file
if(TARGET hiraku_host)
  add_executable(test_statsview host/test_statsview.cpp)
  target_link_libraries(test_statsview PRIVATE hiraku_host)
  add_test(NAME host_statsview COMMAND test_statsview ${CMAKE_CURRENT_BINARY_DIR}/statsview)
  # the boxes over a game's picture in the launcher's look (runtime/host/uibox.h)
  add_executable(test_uibox host/test_uibox.cpp)
  target_link_libraries(test_uibox PRIVATE hiraku_host)
  add_test(NAME host_uibox COMMAND test_uibox ${CMAKE_CURRENT_BINARY_DIR})
  # The game menu and the way out to it (runtime/menu/menu.h, runtime/host/menukey.h): the hold, the
  # launcher's list, and that every cabinet family the games name can drive the menu
  add_executable(test_menu menu/test_menu.cpp)
  target_link_libraries(test_menu PRIVATE hiraku_menu hiraku_shell)
  add_test(NAME game_menu COMMAND test_menu ${CMAKE_SOURCE_DIR}/games ${CMAKE_CURRENT_BINARY_DIR})
  # ... and hiraku-menu itself, driven by a script as an IIDX cabinet drives it: the game it gives
  add_test(NAME game_menu_pick COMMAND hiraku-menu --list ${CMAKE_CURRENT_SOURCE_DIR}/menu/list.txt --family iidx
           --script ${CMAKE_CURRENT_SOURCE_DIR}/menu/pick.his --frames 400 --out ${CMAKE_CURRENT_BINARY_DIR}/menu-run)
  set_tests_properties(game_menu_pick PROPERTIES PASS_REGULAR_EXPRESSION "menu: chosen iidx16")
endif()

# A sound device that stops taking the sound (a USB card pulled out, a driver to restart): when it
# counts as stopped and what is opened in its place (runtime/host/soundwatch.h), then the ALSA back
# end with a pretend libasound whose card the test takes away (host/fake_asound.c; HIRAKU loads
# libasound by name at run time, so LD_LIBRARY_PATH puts the pretend one first)
add_executable(test_soundwatch host/test_soundwatch.cpp)
target_include_directories(test_soundwatch PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME host_soundwatch COMMAND test_soundwatch)
if(TARGET hiraku_host AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
  add_library(fake_asound SHARED host/fake_asound.c)
  set_target_properties(fake_asound PROPERTIES PREFIX "lib" OUTPUT_NAME "asound" SUFFIX ".so.2"
                        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake-asound)
  if(HK_SANITIZE OR HK_SANITIZE_THREAD)   # (it stands for the system's library, which isn't built with them either: HIRAKU
    target_compile_options(fake_asound PRIVATE -fno-sanitize=all)   # calls it by its own declarations)
  endif()
  add_executable(test_alsa_lost host/test_alsa_lost.cpp)
  target_link_libraries(test_alsa_lost PRIVATE hiraku_host)
  add_dependencies(test_alsa_lost fake_asound)
  add_test(NAME host_alsa_lost COMMAND test_alsa_lost ${CMAKE_CURRENT_BINARY_DIR}/fake-asound/gone ${CMAKE_CURRENT_BINARY_DIR}/fake-asound/stuck)
  set_tests_properties(host_alsa_lost PROPERTIES TIMEOUT 60 ENVIRONMENT
    "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/fake-asound;FAKE_ASOUND_GONE=${CMAKE_CURRENT_BINARY_DIR}/fake-asound/gone;FAKE_ASOUND_STUCK=${CMAKE_CURRENT_BINARY_DIR}/fake-asound/stuck")
endif()

# Smooth motion's in-between pictures: the scroll guess (include/hiraku/scroll_guess.h)
add_executable(test_scroll_guess smooth/test_scroll_guess.cpp)
target_include_directories(test_scroll_guess PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME scroll_guess COMMAND test_scroll_guess)

# e-AMUSEMENT (docs/EAMUSE3.md). runtime/eamuse/kbin.h: binary XML and its LZ77
add_executable(test_kbin eamuse/test_kbin.cpp ${CMAKE_SOURCE_DIR}/runtime/eamuse/kbin.cpp)
target_include_directories(test_kbin PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME kbin COMMAND test_kbin)
# hiraku-eamuse's answers, from made-up records: one card for all (pop'n 12-18), pop'n 16-18's records
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT WIN32)
  add_test(NAME eamuse_one_card COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_onecard.py $<TARGET_FILE:hiraku-eamuse>)
  add_test(NAME eamuse_pc_records COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_pcrecords.py $<TARGET_FILE:hiraku-eamuse>)
  add_test(NAME eamuse_players_kept COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_players_kept.py $<TARGET_FILE:hiraku-eamuse>)
  # ... several servers on one data folder, and the bounds on what a server keeps
  add_test(NAME eamuse_shared_folder COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_shared_folder.py $<TARGET_FILE:hiraku-eamuse>)
  add_test(NAME eamuse_iidx_records COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_iidxrecords.py $<TARGET_FILE:hiraku-eamuse>)
  # ... every game family's calls against the answers recorded for them (eamuse/transcripts/), and the
  # data folder's files as they leave it: what work on the server must not change
  add_test(NAME eamuse_transcripts COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_transcripts.py $<TARGET_FILE:hiraku-eamuse>)
  # ... the same against hiraku-server and its database (the database exported at each file's end), and
  # its backup while it serves, two programs writing it at once
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    add_test(NAME eamuse_transcripts_sqlite COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_transcripts.py $<TARGET_FILE:hiraku-server>)
    add_test(NAME eamuse_server_db COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_server_db.py $<TARGET_FILE:hiraku-server>)
    # ... its accounts, cards, arcades and machines (server/accounts.h)
    add_test(NAME eamuse_accounts COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_accounts.py $<TARGET_FILE:hiraku-server>)
  endif()
  # ... what it keeps read back (players, names, scores) and the owner's settings changed (--players, --set...)
  add_test(NAME eamuse_readers COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_readers.py $<TARGET_FILE:hiraku-eamuse>)
  # ... and calls no game would send (it listens on a network): answered or dropped, the server goes on
  add_test(NAME eamuse_malformed COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_malformed.py $<TARGET_FILE:hiraku-eamuse>)
  # ... told which cabinets to answer (cabinets.txt) and which addresses to take (--allow); the relay's
  # port is the one pc_vsock uses
  add_test(NAME eamuse_access COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_access.py $<TARGET_FILE:hiraku-eamuse>)
  set_tests_properties(eamuse_access PROPERTIES RESOURCE_LOCK relay_5740 TIMEOUT 120)
endif()
# A server's key (runtime/shell/keyed.h): the standard pieces it is made of against their standards'
# own examples, the connection's hellos and records without a network, then hiraku-eamuse with a key
# and cabinets with it, without it and with another (key_client: a cabinet's side, for that test)
add_executable(test_crypt sys/test_crypt.cpp)
target_link_libraries(test_crypt PRIVATE hiraku_sys)
add_test(NAME sys_crypt COMMAND test_crypt)
add_executable(test_keyed sys/test_keyed.cpp)
target_link_libraries(test_keyed PRIVATE hiraku_sys)
add_test(NAME sys_keyed COMMAND test_keyed)
# The waits (shell/sys.h): a tenth of a millisecond is waited for, two milliseconds aren't sixteen
add_executable(test_sleep sys/test_sleep.cpp)
target_link_libraries(test_sleep PRIVATE hiraku_sys Threads::Threads)
add_test(NAME sys_sleep COMMAND test_sleep)
set_tests_properties(sys_sleep PROPERTIES RUN_SERIAL TRUE)   # (it measures times)
add_executable(key_client eamuse/key_client.cpp)
target_link_libraries(key_client PRIVATE hiraku_eamuse)
# the server's LZSS packer: the same bytes as the plain search it was, far sooner
add_executable(test_lzss eamuse/test_lzss.cpp)
target_link_libraries(test_lzss PRIVATE hiraku_eamuse)
add_test(NAME eamuse_lzss COMMAND test_lzss)
if(Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME eamuse_key COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_key.py $<TARGET_FILE:hiraku-eamuse> $<TARGET_FILE:key_client>)
  set_tests_properties(eamuse_key PROPERTIES RESOURCE_LOCK relay_5740 TIMEOUT 180)
endif()
# The lists a server on a network keeps (runtime/eamuse/access.h): addresses, cabinets by PCBID
add_executable(test_access_lists eamuse/test_access_lists.cpp)
target_include_directories(test_access_lists PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME eamuse_access_lists COMMAND test_access_lists)
# One game's e-AMUSEMENT folder moved into the shared one (runtime/eamuse/merge.h): each card keeps its own players
add_executable(test_merge eamuse/test_merge.cpp)
target_include_directories(test_merge PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_merge PRIVATE hiraku_sys)
add_test(NAME eamuse_folder_merge COMMAND test_merge)
# What the server keeps (runtime/eamuse/store.h, tables.h): every table's file name, keys refused, the file limit
add_executable(test_store eamuse/test_store.cpp)
target_link_libraries(test_store PRIVATE hiraku_eamuse)
add_test(NAME eamuse_store COMMAND test_store)
# ... and the database back end of hiraku-server (runtime/eamuse/store_sqlite.h), with the system's SQLite 3
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  target_link_libraries(test_store PRIVATE hiraku_easqlite)
  target_compile_definitions(test_store PRIVATE HK_TEST_SQLITE=1)
endif()
# NET TAISEN's lobby (runtime/eamuse/lobby.h): what it holds is bounded
add_executable(test_lobby eamuse/test_lobby.cpp ${CMAKE_SOURCE_DIR}/runtime/eamuse/lobby.cpp)
target_include_directories(test_lobby PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME eamuse_lobby_bounds COMMAND test_lobby)
# The PC games' NET TAISEN (runtime/pc/vsock.c) between two processes on one relay, under Wine
find_program(HK_WINE wine)
if(TARGET hiraku-pc-test-vsock AND HK_WINE AND (NOT WIN32 OR CMAKE_CROSSCOMPILING))   # (a Windows build: its own server)
  add_test(NAME pc_vsock COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/pc/test_vsock.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(pc_vsock PROPERTIES RESOURCE_LOCK relay_5740 TIMEOUT 120)
endif()
# hiraku-pc with a game of HIRAKU's own and no game's data (tests/pc/selfgame.c): the board's DLL
# hooked into it, its pictures in step, its pad, its sound, its files by drive letters, under
# Wine. Wine draws through X: skipped where there is no display
if(TARGET hiraku-pc-selfgame AND TARGET hiraku-pc AND HK_WINE AND (NOT WIN32 OR CMAKE_CROSSCOMPILING))
  add_test(NAME pc_selfgame COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/pc/test_selfgame.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(pc_selfgame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
endif()

# The names a PC game is given for folders its code page can't write (runtime/pc/gamepath.h under
# Wine; on Windows hk::sys::plain_path, which this checks when run there)
add_executable(test_gamepath pc/test_gamepath.cpp)
target_include_directories(test_gamepath PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_gamepath PRIVATE hiraku_sys)
add_test(NAME pc_game_paths COMMAND test_gamepath)

# The PC game's process (runtime/pc/process.h): ended with hiraku-pc however hiraku-pc ends, stopped alone
add_executable(test_game_process pc/test_game_process.cpp ${CMAKE_SOURCE_DIR}/runtime/pc/process.cpp)
target_include_directories(test_game_process PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME pc_game_process COMMAND test_game_process)
set_tests_properties(pc_game_process PROPERTIES TIMEOUT 120)

# Saves written whole or not at all (runtime/shell/sys.h, write_file): NVRAM, cards, the e-AMUSEMENT
# server's files
add_executable(test_will_need sys/test_will_need.cpp)
target_link_libraries(test_will_need PRIVATE hiraku_sys Threads::Threads)
add_test(NAME sys_will_need COMMAND test_will_need)

add_executable(test_write_file sys/test_write_file.cpp)
target_link_libraries(test_write_file PRIVATE hiraku_sys)
add_test(NAME save_write_file COMMAND test_write_file)
# A machine's saved memories written while a game is played (runtime/shell/saves.h): a set of files is
# the folder's whole or not at all, however the program ends
add_executable(test_saves sys/test_saves.cpp)
target_include_directories(test_saves PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_saves PRIVATE hiraku_sys)
add_test(NAME save_sets COMMAND test_saves)
set_tests_properties(save_sets PROPERTIES TIMEOUT 120)
# A file one process has until it ends (runtime/shell/sys.h, lock_file): a PC game's save folder
add_executable(test_lock_file sys/test_lock_file.cpp)
target_include_directories(test_lock_file PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_lock_file PRIVATE hiraku_sys)
add_test(NAME save_folder_lock COMMAND test_lock_file)
# A file held by one holder at a time, each waiting its turn (runtime/shell/sys.h, FileLock): the servers' data folder
add_executable(test_file_lock sys/test_file_lock.cpp)
target_include_directories(test_file_lock PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_file_lock PRIVATE hiraku_sys)
add_test(NAME folder_file_lock COMMAND test_file_lock)
# Paths with letters outside ASCII through the plain-char file functions, arguments and environment
# (on Windows: what cmake/windows/utf8.manifest is for)
add_executable(test_utf8_paths sys/test_utf8_paths.cpp)
target_include_directories(test_utf8_paths PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_utf8_paths PRIVATE hiraku_sys)
add_test(NAME utf8_paths COMMAND test_utf8_paths)

# The problem report's cleaner (launcher/cleaner.h): names, folders, cards and servers gone from a log
if(Qt6_FOUND)
  add_executable(test_cleaner launcher/test_cleaner.cpp ${CMAKE_SOURCE_DIR}/launcher/cleaner.cpp)
  target_include_directories(test_cleaner PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
  target_link_libraries(test_cleaner PRIVATE Qt6::Core)
  add_test(NAME report_cleaner COMMAND test_cleaner)
  # The run logs kept (launcher/runlogs.h): the newest by when they were written, a PC's clock set back or not
  add_executable(test_runlogs launcher/test_runlogs.cpp)
  target_include_directories(test_runlogs PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
  target_link_libraries(test_runlogs PRIVATE Qt6::Core)
  add_test(NAME launcher_run_logs COMMAND test_runlogs ${CMAKE_CURRENT_BINARY_DIR})
  # the game menu's list and choice, the launcher's side (launcher/gamemenu.h)
  add_executable(test_gamemenu launcher/test_gamemenu.cpp)
  target_include_directories(test_gamemenu PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
  target_link_libraries(test_gamemenu PRIVATE Qt6::Core)
  add_test(NAME launcher_game_menu COMMAND test_gamemenu ${CMAKE_CURRENT_BINARY_DIR})
  # the game menu's text in the launcher's fonts: made by the launcher's side, read by the menu's
  # (launcher/menufont.h, runtime/menu/font.h)
  if(TARGET hiraku_menu AND TARGET Qt6::Gui)
    add_executable(test_menufont launcher/test_menufont.cpp ${CMAKE_SOURCE_DIR}/launcher/menufont.cpp)
    target_include_directories(test_menufont PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_menufont PRIVATE Qt6::Gui hiraku_menu)
    add_test(NAME launcher_menu_font COMMAND test_menufont ${CMAKE_CURRENT_BINARY_DIR})
  endif()
  # Windows' power plan while a game runs, the launcher's steps (launcher/powerplan.h)
  add_executable(test_powerplan launcher/test_powerplan.cpp ${CMAKE_SOURCE_DIR}/launcher/powerplan.cpp)
  target_include_directories(test_powerplan PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
  target_link_libraries(test_powerplan PRIVATE Qt6::Core)
  if(WIN32)
    target_link_libraries(test_powerplan PRIVATE powrprof)
  endif()
  add_test(NAME launcher_power_plan COMMAND test_powerplan ${CMAKE_CURRENT_BINARY_DIR})
  # the games as cards (launcher/gamecards.h): their shelves, and a game picked there or in the list
  if(TARGET Qt6::Widgets)
    add_executable(test_gamecards launcher/test_gamecards.cpp ${CMAKE_SOURCE_DIR}/launcher/gamecards.cpp
      ${CMAKE_SOURCE_DIR}/launcher/games.cpp ${CMAKE_SOURCE_DIR}/launcher/config.cpp ${CMAKE_SOURCE_DIR}/launcher/theme.cpp)
    set_target_properties(test_gamecards PROPERTIES AUTOMOC ON)
    target_include_directories(test_gamecards PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_gamecards PRIVATE Qt6::Widgets)
    add_test(NAME launcher_game_cards COMMAND test_gamecards ${CMAKE_CURRENT_BINARY_DIR}/gamecards)
    # the launcher's icons (third_party/mdi) and switches (launcher/theme.h)
    add_executable(test_theme launcher/test_theme.cpp ${CMAKE_SOURCE_DIR}/launcher/theme.cpp ${CMAKE_SOURCE_DIR}/launcher/config.cpp)
    target_include_directories(test_theme PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_theme PRIVATE Qt6::Widgets)
    add_test(NAME launcher_theme COMMAND test_theme)
  endif()
endif()

# Mods' [[text]]: a string found in memory and its replacement (runtime/host/mods.h)
add_executable(test_find_text mods/test_find_text.cpp)
target_link_libraries(test_find_text PRIVATE hiraku_shell)
add_test(NAME mods_text COMMAND test_find_text)
# ... and their [[patch]]es where two mods change the same bytes: the first in the list wins
add_executable(test_patch_order mods/test_patch_order.cpp)
target_link_libraries(test_patch_order PRIVATE hiraku_shell)
add_test(NAME mods_patch_order COMMAND test_patch_order)
# tools/ingest.py (Python 3.11 or later: tomllib): a Konami Python game waiting for its BIOS and keys,
# finished when they're dropped
if(Python3_Interpreter_FOUND AND Python3_VERSION VERSION_GREATER_EQUAL 3.11)
  add_test(NAME ingest_python_support COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ingest_python.py)
endif()
# tools/ingest.py --place: a game's folder moved into dumps/ or read where it is
if(Python3_Interpreter_FOUND AND Python3_VERSION VERSION_GREATER_EQUAL 3.11)
  add_test(NAME ingest_place COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ingest_place.py)
  # ... and its walk of a dropped folder: links followed, but not up the tree or out through a Wine prefix's drives
  if(NOT WIN32)
    add_test(NAME ingest_walk COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ingest_walk.py)
  endif()
endif()
# tools/update.py download: a package the release gives no SHA-256 for, or another's, isn't unpacked
# tools/unlock.py (it edits a game's saved memory) over every games/*/unlocks.toml and saves made
# up; tools/mameset.py (which MAME set a file is) against a MAME made up. No game's data.
if(Python3_Interpreter_FOUND AND Python3_VERSION VERSION_GREATER_EQUAL 3.11 AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME tools_unlock COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_unlock.py)
  add_test(NAME tools_mameset COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_mameset.py)
endif()
# The Python tools read over without being run (tools/lint.py): each compiles, with warnings as
# errors, and reads no name it doesn't define; and the lint's own test
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME tools_lint COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_lint.py)
endif()
if(Python3_Interpreter_FOUND AND NOT WIN32)
  add_test(NAME update_download COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_update.py)
  # ... apply and recover: killed at any file, the folder is put right as the old version or the new, never a mix
  add_test(NAME update_apply COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_update_apply.py $<TARGET_FILE:test_file_lock>)
endif()
# tools/release-fetch.sh: a release's download is used only if it is the file tools/release-sums.txt names
find_program(HK_BASH bash)
find_program(HK_CURL curl)
if(HK_BASH AND HK_CURL AND NOT WIN32)
  add_test(NAME release_fetch COMMAND ${HK_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_release_fetch.sh ${CMAKE_SOURCE_DIR})
endif()
# tools/runpath.py: a packaged program's run path keeps only the package's own folders
find_program(HK_READELF readelf)
if(Python3_Interpreter_FOUND AND HK_READELF AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME package_run_paths COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_runpath.py ${CMAKE_C_COMPILER})
endif()
# tools/package-files.sh: a package takes what git tracks in the repo's folders, nothing else in a checkout
find_program(HK_GIT git)
find_program(HK_BASH bash)
if(HK_GIT AND HK_BASH AND NOT WIN32)
  add_test(NAME package_files COMMAND ${HK_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_package_files.sh ${CMAKE_SOURCE_DIR})
endif()
# tools/ingest.py: a System 573 game's disks without its MAME set (kept for it), then a disk alone
if(Python3_Interpreter_FOUND AND Python3_VERSION VERSION_GREATER_EQUAL 3.11 AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME ingest_disks COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ingest_disks.py $<TARGET_FILE:hiraku-chdcat>)
endif()

# The XT446's MEG programs made into the host's code at run time (runtime/jit/, PLAN.md section 45)
# against their interpreter, on programs and states drawn at random: no ROM needed
add_executable(test_meg_jit jit/test_meg_jit.cpp)
target_link_libraries(test_meg_jit PRIVATE hiraku_xt446)
add_test(NAME meg_jit COMMAND test_meg_jit)
# ... and the PlayStation R3000's blocks made at run time (runtime/sys573/mipsjit.cpp) against MAME's
# interpreter: random programs on two bare CPUs, compared every 1/2000 s. No game needed
add_executable(test_r3000_jit jit/test_r3000_jit.cpp)
target_link_libraries(test_r3000_jit PRIVATE hiraku_sys573 hiraku_sys573_blocks)   # (the blocks no game has: empty tables)
add_test(NAME r3000_jit COMMAND test_r3000_jit)

# The GCU's rows (runtime/dev/k057714_rows.h): copied, blended and filled eight pixels at a time,
# against the pixel-by-pixel loops, on runs drawn at random
add_executable(test_gcu_rows gcu/test_gcu_rows.cpp)
target_include_directories(test_gcu_rows PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME gcu_rows COMMAND test_gcu_rows)

# The GCU's in-between pictures (--smooth-motion): a moving note carried on, translucent strips
# resized from their top edge (beatmania III's lane covers) left as the frame draws them
add_executable(test_gcu_between gcu/test_gcu_between.cpp)
target_include_directories(test_gcu_between PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_gcu_between PRIVATE hiraku_rt)
add_test(NAME gcu_between COMMAND test_gcu_between)

# The Firebeat executor's recorder (runtime/core/exec_rec.cpp): which registers an instruction
# names and which it only sets, against the semantics with registers changed at random
add_executable(test_exec_names core/test_exec_names.cpp)
target_link_libraries(test_exec_names PRIVATE hiraku_rt)
add_test(NAME exec_names COMMAND test_exec_names)

# The PlayStation cores' waiting loops (runtime/sys573/idle.cpp): for how many turns a comparison
# keeps its result, against every turn worked out
add_executable(test_idle_model sys573/test_idle_model.cpp)
target_include_directories(test_idle_model PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME sys573_waiting_loops COMMAND test_idle_model)

# The PlayStation GPU's textured rows (runtime/sys573/psx_spans.h) against the pixel-by-pixel loop
# of MAME's psx.cpp, on rows drawn at random (`test_psx_spans --bench` times the two)
add_executable(test_psx_spans sys573/test_psx_spans.cpp)
target_include_directories(test_psx_spans PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME psx_spans COMMAND test_psx_spans)

# A waiting loop's turn judged from its bus cycles (runtime/dev/loop_turns.h) ...
add_executable(test_loop_turns dev/test_loop_turns.cpp)
target_include_directories(test_loop_turns PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME loop_turns COMMAND test_loop_turns)
# ... and MAME's 68000 with such turns counted (runtime/dev/m68000_loops.h) against the same
# processor running every turn: a small program on two bare 68000s. No game needed
add_executable(test_m68000_loops dev/test_m68000_loops.cpp)
target_link_libraries(test_m68000_loops PRIVATE hiraku_twinkle hiraku_twinkle_blocks)   # (the blocks no game has: empty tables)
target_compile_options(test_m68000_loops PRIVATE -Wno-unused-parameter)   # (the shim's stand-ins)
add_test(NAME m68000_loops COMMAND test_m68000_loops)

# Windows: the tests' programs take paths as UTF-8, as HIRAKU's own do (the manifest the root file
# gives to its programs: utf8_paths is about it), and SDL3.dll is beside those that load it
if(WIN32 AND NOT MSVC)
  get_property(HK_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(t ${HK_TEST_TARGETS})
    get_target_property(HK_TARGET_TYPE ${t} TYPE)
    if(HK_TARGET_TYPE STREQUAL "EXECUTABLE")
      target_sources(${t} PRIVATE $<TARGET_OBJECTS:hiraku_utf8>)
    endif()
  endforeach()
  if(TARGET hiraku_host)
    add_custom_target(hiraku_tests_sdl3_dll ALL
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> ${CMAKE_CURRENT_BINARY_DIR})
  endif()
endif()

# HK_TESTS_REQUIRED: tests that must be there (a list of names). A test whose tool is missing is
# otherwise left out without a word, and a build machine that lost the tool goes on passing: the
# workflow names what each of its jobs is to run.
foreach(t ${HK_TESTS_REQUIRED})
  if(NOT TEST ${t})
    message(FATAL_ERROR "tests: ${t} is asked for (HK_TESTS_REQUIRED) and can't be set up here: see tests/CMakeLists.txt for what it needs")
  endif()
endforeach()

# The fuzz targets (HK_FUZZ; tools/fuzz.sh builds and runs them): the parsers that read what
# HIRAKU didn't write
if(HK_FUZZ)
  add_executable(fuzz_kbin fuzz/fuzz_kbin.cpp)
  target_link_libraries(fuzz_kbin PRIVATE hiraku_eamuse)
  add_executable(fuzz_ea fuzz/fuzz_ea.cpp)
  target_link_libraries(fuzz_ea PRIVATE hiraku_eamuse)
  foreach(t fuzz_kbin fuzz_ea)
    target_link_options(${t} PRIVATE -fsanitize=fuzzer)
  endforeach()
  if(TARGET hiraku_menu)
    add_executable(fuzz_menu fuzz/fuzz_menu.cpp)
    target_link_libraries(fuzz_menu PRIVATE hiraku_menu)
    target_link_options(fuzz_menu PRIVATE -fsanitize=fuzzer)
  endif()
  add_executable(fuzz_seeds fuzz/seeds.cpp)
  target_link_libraries(fuzz_seeds PRIVATE hiraku_eamuse)
endif()
