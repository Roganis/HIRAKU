# The e-AMUSEMENT server (runtime/eamuse/), its files listed once: the top CMakeLists.txt (hiraku_eamuse:
# every core's and hiraku-eamuse's) and the Python core's own build (runtime/python/CMakeLists.txt)
# both take this list, so a file added here is in every program. Paths from the checkout's top.
set(HK_EAMUSE_SOURCES
    runtime/eamuse/protocol.cpp runtime/eamuse/remote.cpp
    runtime/eamuse/server.cpp runtime/eamuse/server_rank.cpp runtime/eamuse/server_share.cpp
    runtime/eamuse/server_popn.cpp runtime/eamuse/server_pc.cpp runtime/eamuse/server_hc.cpp
    runtime/eamuse/server_p24.cpp runtime/eamuse/server_iidx.cpp runtime/eamuse/server_gfdmv.cpp
    runtime/eamuse/server_gfdm.cpp
    runtime/eamuse/kbin.cpp runtime/eamuse/lobby.cpp runtime/eamuse/netrelay.cpp)
