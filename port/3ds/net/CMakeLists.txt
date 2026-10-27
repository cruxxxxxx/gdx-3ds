# port/3ds/net/ — track `hwlink`: the hardware dev link (SOC, log link, control listener,
# hb:ldr self-relaunch). Register new sources HERE, never in port/CMakeLists.txt.
#
# 3DS-only: there is no host equivalent of soc:U / hb:ldr, and nothing in the host stub
# build calls into this. The host configure therefore gets an empty INTERFACE target so
# `target_link_libraries(... gdx3ds_net)` in the parent stays unconditional.
if(NINTENDO_3DS)
    add_library(gdx3ds_net STATIC
        gdx3ds_net.c
        # mp-spike: [net] mp = host|join, deterministic-lockstep multiplayer (DEFAULT OFF).
        # The protocol core (port/gdx_mp_lockstep.c) is portable and compiled into gdx3ds_game.
        gdx3ds_mp.c
        gdx3ds_mp_udp.c
        # mp-online: NEARBY + [net] rendezvous multiplayer up to 4 consoles (default on, idle until used).
        # The transport (p2p), the controller (online) and the VS race glue (game); the
        # protocol halves (GXRV/GXLB/link state, the lobby) are pure and host-tested.
        gdx3ds_mp_p2p.c
        gdx3ds_mp_online.c
        gdx3ds_mp_game.c
        # mp-timesync: frame-advantage pacing ([net] mp_timesync); the core is pure (below).
        gdx3ds_mp_timesync.c
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_timesync.c"
        # card-3: the per-race A/B of mp_timesync_max_ms / mp_ndm_exclusive; the core is pure.
        gdx3ds_mp_netab.c
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_netab.c"
        # NEARBY (same WiFi): the beacon listener / announcer behind gdx3ds_mp_discovery.h.
        gdx3ds_mp_discovery.c
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_online.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_lobby.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_remove.c"
        # mp-menus #10: the shared pause's hold-to-confirm (pure).
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_pause.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_lan.c"
        # mp-online over local wireless: node endpoints, the merged NEARBY list (pure).
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_uds_bridge.c"
        # mp-uds: 3DS local wireless transport + NEARBY discovery (used only with
        # [net] mp_transport=uds|auto; the beacon + drop-out halves are pure, in gdx3ds_glue).
        gdx3ds_mp_uds.c
        # The UDS service thread's SPSC rings + INPUT coalescing scheduler (pure, host-tested).
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_txq.c"
        # The mp-online socket service thread ([net] mp_soc_thread): the thread, and its loop
        # body over those rings (pure, host-tested).
        gdx3ds_mp_socthr.c
        gdx3ds_mp_radio.c
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_mp_socsvc.c"
        # NETTICK: per-kind latency of the network sysmodule calls ([mpipc]).
        gdx3ds_mp_ipcstat.c
        # n60lead hwlink round: [net] enabled's default-resolution logic, split out to
        # port/gdx_net_enabled_default.c so it is host-testable (this TU is 3DS-only --
        # see port/tests/gdx_net_enabled_default_host_tests.c + tools/ci-3ds.sh).
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_net_enabled_default.c")
    # gdx3ds_config.h + gdx3ds_filelog.h come through gdx3ds_os's PUBLIC include dirs;
    # port/gdx_net_enabled_default.h is a bare top-level port/ header (same convention
    # gdx3ds_bottomhud.c uses for port/gdx_hud_placement.h).
    target_include_directories(gdx3ds_net PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../..")
    target_link_libraries(gdx3ds_net PRIVATE gdx3ds_os)
    # Same build-time build id the FPS-HUD bakes (branch@hash of HEAD, refreshed every
    # build -- see the parent CMakeLists.txt's gdx3ds_build_id_header target, hoisted above
    # this add_subdirectory() for exactly this): PING and the "[net] link up" line report
    # it, which is how tools/dev/hw.py reload proves a NEW process came back rather than
    # the old one.
    target_include_directories(gdx3ds_net PRIVATE "${GDX3DS_BUILD_ID_HEADER_DIR}")
    add_dependencies(gdx3ds_net gdx3ds_build_id_header)
    # MP30: the sim-ABI word (gdx3ds_mp_online.c gdx3ds_mp_sim_abi) carries the build flavour, so
    # an Expansion Kit build and a shipped build of one commit refuse each other in the lobby.
    if(GDX_3DS_EXPANSION_KIT)
        target_compile_definitions(gdx3ds_net PRIVATE GDX3DS_NET_EK=1)
    endif()
else()
    add_library(gdx3ds_net INTERFACE)
endif()

target_include_directories(gdx3ds_net INTERFACE .)
