# SPDX-License-Identifier: GPL-3.0-or-later
# The Konami Python core (PLAN.md §23): PCSX2 Reliquary's emulator hosted by HIRAKU's shell.
#
# This file is not part of HIRAKU's own CMake build. PCSX2's CMake assumes it is the
# top-level project, so it is the other way round: Reliquary's build (with HIRAKU's
# patch, oracle/pcsx2/reliquary-oracle.patch) adds this directory when configured with
# -DHIRAKU_DIR=<this checkout>, as oracle/pcsx2/build-reliquary.sh does. The binaries
# land next to pcsx2-qt, in Reliquary's build/bin/, where its resources/ are.

set(HK ${HIRAKU_DIR})

# Windows (PLAN.md section 26, phase 4): PCSX2 builds there with clang-cl, without /EHsc and with
# _HAS_EXCEPTIONS=0 for every target (cmake/BuildParameters.cmake). This directory's targets take
# exceptions back, as -fexceptions does below on Linux: the definition is dropped here, before
# they are made, and HK_PY_FLAGS adds /EHsc.
if(MSVC)
	get_directory_property(HK_PY_DEFS COMPILE_DEFINITIONS)
	list(REMOVE_ITEM HK_PY_DEFS _HAS_EXCEPTIONS=0)
	set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${HK_PY_DEFS}")
endif()

# HIRAKU's shell: the same sources as HIRAKU's hiraku_shell, hiraku_cabinet and hiraku_host
add_library(hk_python_shell STATIC
	${HK}/runtime/shell/cabinet.cpp
	${HK}/runtime/shell/sys.cpp
	${HK}/runtime/shell/net.cpp
	${HK}/runtime/shell/overlay.cpp
	${HK}/runtime/host/hiddesc.cpp
	${HK}/runtime/host/script.cpp
	${HK}/runtime/host/macro.cpp
	${HK}/runtime/host/mods.cpp
	${HK}/runtime/host/pythondisk.cpp
	${HK}/runtime/host/png.cpp
	${HK}/runtime/host/acio.cpp
	${HK}/runtime/host/readers.cpp
	${HK}/runtime/host/bio2.cpp
	${HK}/runtime/host/popnio.cpp
	${HK}/runtime/host/hidout.cpp
	${HK}/runtime/host/window.cpp
	${HK}/runtime/host/alsa.cpp
	${HK}/runtime/host/notice.cpp
	${HK}/runtime/host/lampview.cpp
	${HK}/runtime/host/shaderview.cpp
	${HK}/runtime/host/picture.cpp
	${HK}/runtime/host/input.cpp
	${HK}/runtime/host/lights.cpp
	${HK}/runtime/host/pacing.cpp)
target_include_directories(hk_python_shell PUBLIC ${HK}/runtime ${HK}/include)
target_compile_definitions(hk_python_shell PUBLIC HK_WITH_SDL=1)
target_link_libraries(hk_python_shell PUBLIC SDL3::SDL3)
find_package(ZLIB REQUIRED)   # mods' PNG pictures (runtime/host/png.h)
target_link_libraries(hk_python_shell PUBLIC ZLIB::ZLIB)
if(WIN32)   # as HIRAKU's own build: Winsock (shell/net.h); WASAPI exclusive mode and ASIO (host/wasapi.h, asio.h)
	target_sources(hk_python_shell PRIVATE ${HK}/runtime/host/wasapi.cpp ${HK}/runtime/host/asio.cpp ${HK}/runtime/host/dwm.cpp)
	target_link_libraries(hk_python_shell PUBLIC ws2_32 PRIVATE ole32 avrt uuid)
	# MSVC's <windows.h> has min and max macros in C++ too (mingw's only in C): asio.cpp and
	# wasapi.cpp call std::max
	target_compile_definitions(hk_python_shell PRIVATE NOMINMAX)
endif()
# NFC card readers through PC/SC (runtime/host/readers.h, --scard): optional, as in HIRAKU's build
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
	pkg_check_modules(HK_PCSC QUIET IMPORTED_TARGET libpcsclite)
endif()
# HK_PCSC_DLOPEN (a release's, tools/release-python-linux.sh): libpcsclite loaded at run time, as
# in HIRAKU's own build (runtime/host/pcsc_load.cpp)
option(HK_PCSC_DLOPEN "load libpcsclite at run time instead of linking it" OFF)
if(HK_PCSC_DLOPEN)
	find_path(HK_PCSC_INCLUDE PCSC/winscard.h)
endif()
if(WIN32)
	target_link_libraries(hk_python_shell PRIVATE winscard)   # Windows' own PC/SC
	target_compile_definitions(hk_python_shell PRIVATE HK_WITH_PCSC=1)
elseif(HK_PCSC_DLOPEN AND HK_PCSC_INCLUDE)
	target_sources(hk_python_shell PRIVATE ${HK}/runtime/host/pcsc_load.cpp)
	target_include_directories(hk_python_shell PRIVATE ${HK_PCSC_INCLUDE} ${HK_PCSC_INCLUDE}/PCSC)
	target_link_libraries(hk_python_shell PRIVATE ${CMAKE_DL_LIBS})
	target_compile_definitions(hk_python_shell PRIVATE HK_WITH_PCSC=1)
elseif(HK_PCSC_FOUND)
	target_link_libraries(hk_python_shell PRIVATE PkgConfig::HK_PCSC)
	target_compile_definitions(hk_python_shell PRIVATE HK_WITH_PCSC=1)
endif()
if(NOT WIN32)
	find_path(HK_WAYLAND_CLIENT_INCLUDE wayland-client.h)
	find_library(HK_WAYLAND_CLIENT_LIB wayland-client)
endif()
if(HK_WAYLAND_CLIENT_INCLUDE AND HK_WAYLAND_CLIENT_LIB)
	target_sources(hk_python_shell PRIVATE ${HK}/third_party/wayland/presentation-time-protocol.c
	                                       ${HK}/third_party/wayland/fifo-v1-protocol.c
	                                       ${HK}/third_party/wayland/content-type-v1-protocol.c)
	target_include_directories(hk_python_shell PRIVATE ${HK}/third_party/wayland ${HK_WAYLAND_CLIENT_INCLUDE})
	target_link_libraries(hk_python_shell PRIVATE ${HK_WAYLAND_CLIENT_LIB})
	target_compile_definitions(hk_python_shell PRIVATE HK_WITH_WAYLAND=1)
endif()
# HIRAKU's code, HIRAKU's warnings; PCSX2's global options still apply, except that
# HIRAKU's parsers use exceptions (PCSX2 builds with -fno-exceptions)
if(MSVC)
	# clang-cl's -Wall is -Weverything: /W4 is -Wall -Wextra there. No strict aliasing is its
	# default. /EHsc: exceptions (see _HAS_EXCEPTIONS above).
	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(HK_PY_FLAGS /W4 -Wno-unused-parameter /EHsc)
	else()
		set(HK_PY_FLAGS /W4 /wd4100 /EHsc)
	endif()
else()
	set(HK_PY_FLAGS -Wall -Wextra -fno-strict-aliasing -Wno-unused-parameter -fexceptions)
endif()
target_compile_options(hk_python_shell PRIVATE ${HK_PY_FLAGS})
set_target_properties(hk_python_shell PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The Python core and HIRAKU's main(), compiled once. hiraku-python runs any Python game (--game ID,
# or its name); hiraku-<id> is a link to it (runtime/shell/built.h), made by the target of that name.
# Windows (no links): a binary per game, of the same objects and a one-line file naming the game
add_library(hk_python_core OBJECT ${HK}/runtime/main.cpp ${HK}/runtime/python/core.cpp ${HK}/runtime/python/cardunit.cpp
	${HK}/runtime/python/icca.cpp ${HK}/runtime/python/eanet.cpp ${HK}/runtime/python/ethnet.cpp ${HK}/runtime/python/gfdmcard.cpp
	${HK}/runtime/python/lancable.cpp ${HK}/runtime/python/netrelay.cpp ${HK}/runtime/python/lobby.cpp
	${HK}/runtime/python/easerver.cpp ${HK}/runtime/python/easerver_rank.cpp ${HK}/runtime/python/easerver_gfdm.cpp
	${HK}/runtime/python/easerver_popn.cpp ${HK}/runtime/python/easerver_share.cpp ${HK}/runtime/python/easerver_pc.cpp
	${HK}/runtime/python/easerver_hc.cpp
	${HK}/runtime/python/easerver_p24.cpp
	${HK}/runtime/python/easerver_iidx.cpp
	${HK}/runtime/python/kbin.cpp ${HK}/runtime/eamuse/protocol.cpp ${HK}/runtime/eamuse/remote.cpp)
target_compile_definitions(hk_python_core PRIVATE HK_ROOT="${HK}")
target_include_directories(hk_python_core PRIVATE ${HK}/runtime "${CMAKE_BINARY_DIR}/common/include" "${CMAKE_SOURCE_DIR}/pcsx2")
target_link_libraries(hk_python_core PRIVATE hk_python_shell PCSX2_FLAGS PCSX2)
target_compile_options(hk_python_core PRIVATE ${HK_PY_FLAGS})

function(hiraku_python_game id)
	if(ARGV1 STREQUAL "CORE")
		set(built ${HK}/runtime/shell/built_none.cpp)
	elseif(NOT WIN32)
		add_custom_target(hiraku-${id} ALL
			COMMAND ${CMAKE_COMMAND} -E create_symlink hiraku-python $<TARGET_FILE_DIR:hiraku-python>/hiraku-${id}
			DEPENDS hiraku-python VERBATIM)
		return()
	else()
		set(built ${CMAKE_CURRENT_BINARY_DIR}/built/${id}.cpp)
		file(CONFIGURE OUTPUT ${built} CONTENT "#include \"shell/built.h\"\nconst char *const hk::built_game = \"${id}\";\n")
	endif()
	add_executable(hiraku-${id} $<TARGET_OBJECTS:hk_python_core> ${built})
	target_include_directories(hiraku-${id} PRIVATE ${HK}/runtime)
	target_link_libraries(hiraku-${id} PRIVATE hk_python_shell PCSX2_FLAGS PCSX2)
	target_compile_options(hiraku-${id} PRIVATE ${HK_PY_FLAGS})
endfunction()

# hiraku-python: every Python game, as hiraku-djmain and the other cores' own binaries are
hiraku_python_game(python CORE)
hiraku_python_game(popn9)
hiraku_python_game(popn10)
hiraku_python_game(popn11)
hiraku_python_game(popn12)
hiraku_python_game(popn13)
hiraku_python_game(popn14)
# Konami Python 2 (PY-6)
hiraku_python_game(drmnvj)
hiraku_python_game(drmnv2j)
