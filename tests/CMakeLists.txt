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
  # the 10-key's 5, 7 and 3 held at known times: when the game and the way out to the game menu see
  # them, a reader gone quiet, late answers; and a game with no 10-key of its own
  add_test(NAME card_readers_hold COMMAND test_readers hold)
  add_test(NAME card_readers_hold_nokeys COMMAND test_readers hold nokeys)
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
  # ... and a display that holds some of the game's thread's presents: the bursts after them late, and
  # the log's account of each (host/breaks.h) naming the present as what made them late
  add_test(NAME swapchain_held COMMAND test_swapchain held 120 3.0 600)
  # ... a window the display stops showing for a while (Alt+Tab, covered, minimized), then shows again:
  # its pictures presented again at once (the statistics that stood still meanwhile not believed), with
  # and without smooth motion and display sync; a composited desktop's statistics, late after each
  # vblank or brought up to date only by the next present: few pictures left out, never many in a row
  add_test(NAME swapchain_covered_lost COMMAND test_swapchain covered-lost 60 3.0 400 nosmooth)
  add_test(NAME swapchain_covered_live COMMAND test_swapchain covered-live 240 3.0 400)
  add_test(NAME swapchain_covered_jump COMMAND test_swapchain covered-jump 60 3.0 400)
  add_test(NAME swapchain_covered_nosync COMMAND test_swapchain covered-lost 60 3.0 400 nosync)
  add_test(NAME swapchain_occluded COMMAND test_swapchain occluded 60 3.0 400 nosmooth)
  add_test(NAME swapchain_composed COMMAND test_swapchain composed 60 3.0 1500 nosmooth)
  add_test(NAME swapchain_snapshot COMMAND test_swapchain snapshot 60 3.0 1500 nosmooth)
endif()
# Display pacing's judgement of the compositor's deadline (runtime/host/pacing.cpp) from made-up
# presentation feedback, as Wayland's fifo-v1 gives it: windows missed after a late commit leave the
# lead where it was; a modelled display's moving deadline followed in a few raises at 60, 120 and
# 240 Hz; and the old catch-up (HK_PACING_DRAIN=0). (Windows: the swap chain tests)
if(NOT WIN32 AND TARGET hiraku_host)
  add_executable(test_pacing host/test_pacing.cpp)
  target_include_directories(test_pacing PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
  target_link_libraries(test_pacing PRIVATE SDL3::SDL3)
  add_test(NAME host_pacing COMMAND test_pacing)
  add_test(NAME host_pacing_no_drain COMMAND test_pacing)
  set_tests_properties(host_pacing_no_drain PROPERTIES ENVIRONMENT "HK_PACING_DRAIN=0")
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

# The C33 cabinet's small display (runtime/pc/c33video.h: ee'MALL's video board) drawn from a
# made-up board: tiles, palette rows, the bank, a hidden layer, a layer over another, the blank bit
add_executable(test_c33video pc/test_c33video.c ${CMAKE_SOURCE_DIR}/runtime/pc/c33video.c)
target_include_directories(test_c33video PRIVATE ${CMAKE_SOURCE_DIR}/runtime/pc)
add_test(NAME pc_c33_video COMMAND test_c33video)

# A mod's addresses and bytes in a game's DLL loaded away from its base (runtime/pc/imagebase.h):
# made-up PE32 and PE32+ images with a relocation table, "loaded" elsewhere
add_executable(test_imagebase pc/test_imagebase.c ${CMAKE_SOURCE_DIR}/runtime/pc/imagebase.c)
target_include_directories(test_imagebase PRIVATE ${CMAKE_SOURCE_DIR}/runtime/pc)
add_test(NAME pc_imagebase COMMAND test_imagebase)

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
# The launcher's music volume (launcher/musicvolume.h): a volume saved while the slider did nothing
# set back once, the parts' meters scaled by the volume
add_executable(test_music_volume launcher/test_music_volume.cpp)
target_include_directories(test_music_volume PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
add_test(NAME launcher_music_volume COMMAND test_music_volume)

# The video recorder (runtime/host/record.h): made-up frames and sound to a file, read back with the
# same FFmpeg libraries (frames, size, rate, times, pixels, every sound sample, sizes, a queue full)
if(HK_FFMPEG_FOUND)
  add_executable(test_record host/test_record.cpp)
  target_link_libraries(test_record PRIVATE hiraku_record PkgConfig::HK_FFMPEG)
  add_test(NAME host_record COMMAND test_record ${CMAKE_CURRENT_BINARY_DIR}/record)
  set_tests_properties(host_record PROPERTIES SKIP_RETURN_CODE 77)   # (an FFmpeg without the recorder's parts)
endif()
# Screenshots (runtime/host/screenshot.h, F5) and the PNG writer (runtime/host/png.h, encode): made-up
# pictures encoded and decoded back, the files' names and no file written over, a folder that is a
# file, one with letters outside ASCII, the saver's thread; then the files read by Python's own zlib
add_executable(test_screenshot host/test_screenshot.cpp)
target_link_libraries(test_screenshot PRIVATE hiraku_shell)
add_test(NAME host_screenshot COMMAND test_screenshot ${CMAKE_CURRENT_BINARY_DIR}/screenshot)
set_tests_properties(host_screenshot PROPERTIES FIXTURES_SETUP screenshot_files)
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME host_screenshot_png COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/host/check_png.py ${CMAKE_CURRENT_BINARY_DIR}/screenshot)
  set_tests_properties(host_screenshot_png PROPERTIES FIXTURES_REQUIRED screenshot_files)
endif()

# The breaks in the sound sorted by what their frame shows (runtime/host/breaks.h: the exit report's
# classes and the slow-frame lines' late-burst causes) from made-up frames, and the log's line of test
# switches (runtime/shell/switches.h), a secret's value never in it
add_executable(test_breaks host/test_breaks.cpp)
target_include_directories(test_breaks PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME host_breaks COMMAND test_breaks)

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
# ... only in songs: a game's [smooth] word (runtime/shell/smoothgate.h, the [[rate]] condition)
add_executable(test_smoothgate smooth/test_smoothgate.cpp)
target_include_directories(test_smoothgate PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME smoothgate COMMAND test_smoothgate)
# The cores' base volume evened out (runtime/shell/level.h, --even-volume): the families' trims, a
# game.toml's [sound] level_db, and every game in games/ with a family's trim
add_executable(test_level host/test_level.cpp)
target_include_directories(test_level PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME even_volume COMMAND test_level ${CMAKE_SOURCE_DIR}/games)

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
    # ... the players' plays as hiraku-server's stats show them (runtime/eamuse/readers.cpp)
    add_test(NAME eamuse_readers_server COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_readers.py $<TARGET_FILE:hiraku-eamuse> $<TARGET_FILE:hiraku-server>)
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
# ... and hiraku-server's key per machine (machine add, machine key): calls and the relay behind it
if(SQLite3_FOUND AND Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)   # (hiraku-server is made with SQLite 3)
  add_test(NAME eamuse_machine_keys COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_machine_keys.py
           $<TARGET_FILE:hiraku-server> $<TARGET_FILE:key_client> $<TARGET_FILE:hiraku-eamuse>)
  set_tests_properties(eamuse_machine_keys PROPERTIES RESOURCE_LOCK relay_5740 TIMEOUT 180)
  # ... and its JSON API (server/api.h): who calls, each role, cards, stats, keys, settings, limits, no secret out
  add_test(NAME eamuse_api COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_api.py
           $<TARGET_FILE:hiraku-server> $<TARGET_FILE:key_client>)
  set_tests_properties(eamuse_api PROPERTIES TIMEOUT 180)
  # ... and its description (docs/openapi.yaml, docs/SERVER-API.md): the routes, roles, the JSON twin,
  # every answer of a walk through each operation and of test_api.py against the schemas
  add_test(NAME eamuse_openapi COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_openapi.py
           $<TARGET_FILE:hiraku-server> $<TARGET_FILE:key_client> ${CMAKE_SOURCE_DIR})
  set_tests_properties(eamuse_openapi PROPERTIES TIMEOUT 300)
  # ... and its web pages (web/) on the API's port: types, security headers, paths refused, the files' text
  add_test(NAME eamuse_web COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_web.py
           $<TARGET_FILE:hiraku-server> ${CMAKE_SOURCE_DIR}/web)
  set_tests_properties(eamuse_web PROPERTIES TIMEOUT 120)
  # ... its song names' packs (server/names.h): made-up packs loaded, read again, refused; the API's calls
  add_test(NAME eamuse_names COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_names.py $<TARGET_FILE:hiraku-server>)
  set_tests_properties(eamuse_names PROPERTIES TIMEOUT 120)
  # ... and tools/eamuse/names.py's reading and numbering, on tiny made-up tables
  add_test(NAME eamuse_names_tool COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_names_tool.py)
  # ... and run as a service: health, the log (nothing secret), --log-calls, the config file, backups kept, SIGTERM
  add_test(NAME eamuse_service COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_service.py
           $<TARGET_FILE:hiraku-server> $<TARGET_FILE:key_client>)
  set_tests_properties(eamuse_service PROPERTIES TIMEOUT 180)
  # ... its limits and choices: connections at once and from one address, --sync, refusals as kinds,
  # a card as the admin sees it and the tries left before its lock
  add_test(NAME eamuse_server_limits COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_server_limits.py
           $<TARGET_FILE:hiraku-server> ${CMAKE_SOURCE_DIR})
  set_tests_properties(eamuse_server_limits PROPERTIES TIMEOUT 180)
  # ... and under load for 3 seconds (tools/eamuse/load.py: made-up cabinets playing credits, health
  # polled, a backup in the middle; its long runs are docs/SERVER.md's numbers)
  add_test(NAME eamuse_load_smoke COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/eamuse/load.py
           $<TARGET_FILE:hiraku-server> --cabinets 6 --seconds 3 --workers 2 --quiet)
  set_tests_properties(eamuse_load_smoke PROPERTIES TIMEOUT 120)
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
# The mods' files in a PC game's process (runtime/pc/cabinet.c), under Wine: a program of HIRAKU's
# loads the board's DLL and opens, looks at and lists replaced and added files and folders through
# the DLL's hooks (tests/pc/test_modfiles.c)
if(TARGET hiraku-pc-test-modfiles AND HK_WINE AND (NOT WIN32 OR CMAKE_CROSSCOMPILING))
  add_test(NAME pc_mod_files COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/pc/test_modfiles.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(pc_mod_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
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
# An IC card as a player's floppy disk or memory card (runtime/shell/cardmedia.h, --card-media): its
# folder, lock and blank disk; in, out and swap; a change waiting while the game writes; the slots
add_executable(test_card_media sys/test_card_media.cpp)
target_include_directories(test_card_media PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(test_card_media PRIVATE hiraku_rt hiraku_sys)
add_test(NAME card_media COMMAND test_card_media)
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
  # a mod's settings panel, the launcher's side (launcher/modsettings.h): read by the loader's own
  # reader (runtime/host/modsettings.h), its rows, and the settings file written with its other lines kept
  add_executable(test_mod_settings launcher/test_mod_settings.cpp)
  target_include_directories(test_mod_settings PRIVATE ${CMAKE_SOURCE_DIR}/launcher ${CMAKE_SOURCE_DIR}/runtime)
  target_link_libraries(test_mod_settings PRIVATE Qt6::Core)
  add_test(NAME launcher_mod_settings COMMAND test_mod_settings ${CMAKE_CURRENT_BINARY_DIR}/mod-settings-test)
  # the game menu's text in the launcher's fonts: made by the launcher's side, read by the menu's
  # (launcher/menufont.h, runtime/menu/font.h)
  if(TARGET hiraku_menu AND TARGET Qt6::Gui)
    add_executable(test_menufont launcher/test_menufont.cpp ${CMAKE_SOURCE_DIR}/launcher/menufont.cpp)
    target_include_directories(test_menufont PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_menufont PRIVATE Qt6::Gui hiraku_menu)
    add_test(NAME launcher_menu_font COMMAND test_menufont ${CMAKE_CURRENT_BINARY_DIR})
  endif()
  # recording, the launcher's side (launcher/recording.h): Settings' choices as the game's options,
  # the files a conversion takes, the converter's progress
  add_executable(test_recording launcher/test_recording.cpp)
  target_include_directories(test_recording PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
  target_link_libraries(test_recording PRIVATE Qt6::Core)
  add_test(NAME launcher_recording COMMAND test_recording)
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
      ${CMAKE_SOURCE_DIR}/launcher/games.cpp ${CMAKE_SOURCE_DIR}/launcher/config.cpp ${CMAKE_SOURCE_DIR}/launcher/theme.cpp
      ${CMAKE_SOURCE_DIR}/launcher/language.cpp)
    set_target_properties(test_gamecards PROPERTIES AUTOMOC ON)
    target_include_directories(test_gamecards PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_gamecards PRIVATE Qt6::Widgets hiraku_lang)
    add_test(NAME launcher_game_cards COMMAND test_gamecards ${CMAKE_CURRENT_BINARY_DIR}/gamecards)
    # the launcher's icons (third_party/mdi) and switches (launcher/theme.h)
    add_executable(test_theme launcher/test_theme.cpp ${CMAKE_SOURCE_DIR}/launcher/theme.cpp ${CMAKE_SOURCE_DIR}/launcher/config.cpp)
    target_include_directories(test_theme PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_theme PRIVATE Qt6::Widgets)
    add_test(NAME launcher_theme COMMAND test_theme)
    # the controls sets: the beatmania cabinets' one file, or one each (launcher/config.h)
    add_executable(test_controls_sets launcher/test_controls_sets.cpp ${CMAKE_SOURCE_DIR}/launcher/config.cpp)
    target_include_directories(test_controls_sets PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_controls_sets PRIVATE Qt6::Core)
    add_test(NAME launcher_controls_sets COMMAND test_controls_sets ${CMAKE_CURRENT_BINARY_DIR}/controls_sets)
    # the launcher's language (launcher/language.h): its catalogue through Qt's own hook, Qt's
    # standard buttons, plurals, English for a catalogue missing or broken; the binding dialog marked
    add_executable(test_language launcher/test_language.cpp ${CMAKE_SOURCE_DIR}/launcher/language.cpp
      ${CMAKE_SOURCE_DIR}/launcher/capturedialog.cpp ${CMAKE_SOURCE_DIR}/launcher/inputs.cpp)
    set_target_properties(test_language PROPERTIES AUTOMOC ON)
    target_include_directories(test_language PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
    target_link_libraries(test_language PRIVATE Qt6::Widgets SDL3::SDL3 hiraku_lang hiraku_cabinet)
    add_test(NAME launcher_language COMMAND test_language ${CMAKE_CURRENT_BINARY_DIR}/language)
  endif()
endif()

# The interface's catalogues (runtime/lang/lang.h): lookup, fallback, contexts, plurals, argument
# order, broken files, the system's language; and the checkout's lang/*.po each read clean
add_executable(test_lang lang/test_lang.cpp)
target_link_libraries(test_lang PRIVATE hiraku_lang)
add_test(NAME lang_catalogues COMMAND test_lang ${CMAKE_SOURCE_DIR}/lang ${CMAKE_CURRENT_BINARY_DIR}/lang-test)
# ... and lang/ made from the sources as they are: a text marked without tools/lang.py run is noticed
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME lang_extracted COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/lang.py --check)
endif()

# Mods' [[text]]: a string found in memory and its replacement (runtime/host/mods.h)
add_executable(test_find_text mods/test_find_text.cpp)
target_link_libraries(test_find_text PRIVATE hiraku_shell)
add_test(NAME mods_text COMMAND test_find_text)
# ... and their [[patch]]es where two mods change the same bytes: the first in the list wins
add_executable(test_patch_order mods/test_patch_order.cpp)
target_link_libraries(test_patch_order PRIVATE hiraku_shell)
add_test(NAME mods_patch_order COMMAND test_patch_order)
# ... and their files over a folder medium (the PC games' data): replaced, added, left out, the
# first mod winning
add_executable(test_mod_files mods/test_mod_files.cpp)
target_link_libraries(test_mod_files PRIVATE hiraku_shell)
add_test(NAME mods_files COMMAND test_mod_files ${CMAKE_CURRENT_BINARY_DIR}/mod-files-test)
# ... and files and patches chosen by a setting (choices, when, [[files]]), with the settings file
# and --mod-setting read before them
add_executable(test_mod_choose mods/test_mod_choose.cpp)
target_link_libraries(test_mod_choose PRIVATE hiraku_shell)
add_test(NAME mods_choose COMMAND test_mod_choose ${CMAKE_CURRENT_BINARY_DIR}/mod-choose-test)
# ... and the mod API's tables (api, requires, program, [[hook]], [[setting]], [[action]]) on a
# made-up machine, with the sample mod under tests/mods/sample-hook
add_executable(test_mod_api mods/test_mod_api.cpp)
target_link_libraries(test_mod_api PRIVATE hiraku_shell)
add_test(NAME mods_api COMMAND test_mod_api ${CMAKE_CURRENT_BINARY_DIR}/mod-api-test ${CMAKE_CURRENT_SOURCE_DIR}/mods/sample-hook)
# ... and mods' scripts (runtime/host/modscript.h), with the sample under tests/mods/sample-script
add_executable(test_mod_script mods/test_mod_script.cpp)
target_link_libraries(test_mod_script PRIVATE hiraku_shell)
add_test(NAME mods_script COMMAND test_mod_script ${CMAKE_CURRENT_BINARY_DIR}/mod-script-test ${CMAKE_CURRENT_SOURCE_DIR}/mods/sample-script)
# ... and a per_picture patch: a frame's time made the rate's picture's while a [[rate]] is on
add_executable(test_per_picture mods/test_per_picture.cpp)
target_link_libraries(test_per_picture PRIVATE hiraku_shell)
add_test(NAME mods_per_picture COMMAND test_per_picture)
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
# the XT446's H8 made into code at run time against MAME's interpreter (made-up programs, no ROM)
add_executable(test_h8_jit jit/test_h8_jit.cpp)
target_link_libraries(test_h8_jit PRIVATE hiraku_xt446)
add_test(NAME h8_jit COMMAND test_h8_jit)
# ... and the PlayStation R3000's blocks made at run time (runtime/sys573/mipsjit.cpp) against MAME's
# interpreter: random programs on two bare CPUs, compared every 1/2000 s. No game needed
add_executable(test_r3000_jit jit/test_r3000_jit.cpp)
target_link_libraries(test_r3000_jit PRIVATE hiraku_sys573 hiraku_sys573_blocks)   # (the blocks no game has: empty tables)
add_test(NAME r3000_jit COMMAND test_r3000_jit)
# ... and Firebeat's PowerPC 403: the executor's functions made at load (runtime/core/exec_jit.cpp)
# against its interpreter of steps, on random made-up programs
add_executable(test_ppc403_jit jit/test_ppc403_jit.cpp ${CMAKE_SOURCE_DIR}/runtime/core/no_table.c)
target_link_libraries(test_ppc403_jit PRIVATE hiraku_rt)
add_test(NAME ppc403_jit COMMAND test_ppc403_jit ${CMAKE_CURRENT_BINARY_DIR}/ppc403-jit-test)

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
# A mod's hooks on a made-up program (runtime/core/hooks.h): before their instruction in the
# executor and the interpreter, and never skipped by a remembered run (twice: HK_EXEC_MEMO=check)
add_executable(test_exec_hook core/test_exec_hook.cpp ${CMAKE_SOURCE_DIR}/runtime/core/no_table.c)
target_link_libraries(test_exec_hook PRIVATE hiraku_rt)
add_test(NAME exec_hook COMMAND test_exec_hook ${CMAKE_CURRENT_BINARY_DIR}/exec-hook-test)
add_test(NAME exec_hook_memo_check COMMAND test_exec_hook ${CMAKE_CURRENT_BINARY_DIR}/exec-hook-test-check)
set_tests_properties(exec_hook_memo_check PROPERTIES ENVIRONMENT "HK_EXEC_MEMO=check")
# A mod's code patch and the remembered runs (runtime/core/exec.cpp): a caller's run from before a
# patch is not taken after it, the rest still are (twice: HK_EXEC_MEMO=check)
add_executable(test_exec_patch core/test_exec_patch.cpp ${CMAKE_SOURCE_DIR}/runtime/core/no_table.c)
target_link_libraries(test_exec_patch PRIVATE hiraku_rt)
add_test(NAME exec_patch COMMAND test_exec_patch ${CMAKE_CURRENT_BINARY_DIR}/exec-patch-test)
add_test(NAME exec_patch_memo_check COMMAND test_exec_patch ${CMAKE_CURRENT_BINARY_DIR}/exec-patch-test-check)
set_tests_properties(exec_patch_memo_check PROPERTIES ENVIRONMENT "HK_EXEC_MEMO=check")

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

# The System 573's memory card reader and its PlayStation memory cards (runtime/sys573/memcard.h):
# the card's protocol bit by bit, the reader's JVS commands and the cards' files. No game needed
add_executable(test_memcard sys573/test_memcard.cpp)
target_link_libraries(test_memcard PRIVATE hiraku_sys573 hiraku_sys573_blocks hiraku_sys)
add_test(NAME sys573_memory_cards COMMAND test_memcard ${CMAKE_CURRENT_BINARY_DIR}/memcard-test)

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
  if(HK_FFMPEG_DLLS)   # ... and FFmpeg's DLLs (CMakeLists.txt, tools/ffmpeg-min.sh)
    add_custom_target(hiraku_tests_ffmpeg_dlls ALL COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HK_FFMPEG_DLLS} ${CMAKE_CURRENT_BINARY_DIR})
    add_dependencies(hiraku_record hiraku_tests_ffmpeg_dlls)
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
