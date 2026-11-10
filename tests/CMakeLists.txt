# SPDX-License-Identifier: GPL-3.0-or-later

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

# ATA security (SECURITY UNLOCK) on a hard disk, as MAME does it (DJ Main complete MIX on)
add_executable(test_ata_security ata/test_ata_security.cpp)
target_link_libraries(test_ata_security PRIVATE hiraku_ata)
target_include_directories(test_ata_security PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME ata_security COMMAND test_ata_security)

# The picture's shader engine: FrameCount, frame history, feedback (needs a display and OpenGL
# 3.3; skipped without them)
if(TARGET hiraku_host)
  add_executable(test_picture picture/test_picture.cpp)
  target_link_libraries(test_picture PRIVATE hiraku_host)
  add_test(NAME picture_engine COMMAND test_picture)
  set_tests_properties(picture_engine PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Host controls (runtime/host/input.h) with a controller SDL makes up: bindings that share an input,
# a controller unplugged and plugged back in
if(TARGET hiraku_host)
  add_executable(test_bindings input/test_bindings.cpp)
  target_link_libraries(test_bindings PRIVATE hiraku_host)
  add_test(NAME input_bindings COMMAND test_bindings)
endif()

# Smooth motion's in-between pictures: the scroll guess (include/hiraku/scroll_guess.h)
add_executable(test_scroll_guess smooth/test_scroll_guess.cpp)
target_include_directories(test_scroll_guess PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME scroll_guess COMMAND test_scroll_guess)

# e-AMUSEMENT (docs/EAMUSE3.md). runtime/python/kbin.h: binary XML and its LZ77
add_executable(test_kbin eamuse/test_kbin.cpp ${CMAKE_SOURCE_DIR}/runtime/python/kbin.cpp)
target_include_directories(test_kbin PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME kbin COMMAND test_kbin)
# hiraku-eamuse's answers, from made-up records: one card for all (pop'n 12-18), pop'n 16-18's records
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT WIN32)
  add_test(NAME eamuse_one_card COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_onecard.py $<TARGET_FILE:hiraku-eamuse>)
  add_test(NAME eamuse_pc_records COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_pcrecords.py $<TARGET_FILE:hiraku-eamuse>)
  add_test(NAME eamuse_players_kept COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_players_kept.py $<TARGET_FILE:hiraku-eamuse>)
  add_test(NAME eamuse_iidx_records COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_iidxrecords.py $<TARGET_FILE:hiraku-eamuse>)
  # ... and calls no game would send (it listens on a network): answered or dropped, the server goes on
  add_test(NAME eamuse_malformed COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_malformed.py $<TARGET_FILE:hiraku-eamuse>)
  # ... told which cabinets to answer (cabinets.txt) and which addresses to take (--allow); the relay's
  # port is the one pc_vsock uses
  add_test(NAME eamuse_access COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/eamuse/test_access.py $<TARGET_FILE:hiraku-eamuse>)
  set_tests_properties(eamuse_access PROPERTIES RESOURCE_LOCK relay_5740 TIMEOUT 120)
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
# The PC games' NET TAISEN (runtime/pc/vsock.c) between two processes on one relay, under Wine
find_program(HK_WINE wine)
if(TARGET hiraku-pc-test-vsock AND HK_WINE AND NOT WIN32)
  add_test(NAME pc_vsock COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/pc/test_vsock.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(pc_vsock PROPERTIES RESOURCE_LOCK relay_5740 TIMEOUT 120)
endif()

# The PC game's process (runtime/pc/process.h): ended with hiraku-pc however hiraku-pc ends, stopped alone
add_executable(test_game_process pc/test_game_process.cpp ${CMAKE_SOURCE_DIR}/runtime/pc/process.cpp)
target_include_directories(test_game_process PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
add_test(NAME pc_game_process COMMAND test_game_process)
set_tests_properties(pc_game_process PROPERTIES TIMEOUT 120)

# Saves written whole or not at all (runtime/shell/sys.h, write_file): NVRAM, cards, the e-AMUSEMENT
# server's files
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

# The problem report's cleaner (launcher/cleaner.h): names, folders, cards and servers gone from a log
if(Qt6_FOUND)
  add_executable(test_cleaner launcher/test_cleaner.cpp ${CMAKE_SOURCE_DIR}/launcher/cleaner.cpp)
  target_include_directories(test_cleaner PRIVATE ${CMAKE_SOURCE_DIR}/launcher)
  target_link_libraries(test_cleaner PRIVATE Qt6::Core)
  add_test(NAME report_cleaner COMMAND test_cleaner)
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
if(Python3_Interpreter_FOUND AND NOT WIN32)
  add_test(NAME update_download COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_update.py)
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
if(Python3_Interpreter_FOUND AND Python3_VERSION VERSION_GREATER_EQUAL 3.11)
  add_test(NAME ingest_disks COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ingest_disks.py $<TARGET_FILE:hiraku-chdcat>)
endif()
