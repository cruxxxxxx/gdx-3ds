/* port/3ds/net/gdx3ds_mp.h — MP spike (track mp-spike): `[net] mp = host|join`, a 2+-console
 * deterministic-lockstep race over a datagram transport (gdx3ds_mp_transport.h: udp-lan, uds).
 *
 * KILLSWITCH: [net] mp absent / empty / "off" (THE DEFAULT) = this TU is inert: no socket, no
 * allocation, no receipt, gdx_mp_exchange returns 0 on its first int test and the input path
 * is the shipped one byte for byte.
 *
 * ini keys ([net]):
 *   mp            off | host | join
 *   mp_players    2      players in the session (2..8); host is player 0
 *   mp_player     1      joiner: our player index (1..mp_players-1)
 *   mp_delay      2      input delay in ticks (1..8)
 *   mp_hash_every 1      publish/compare the state digest every K ticks
 *   mp_port       27100  host's UDP port; a joiner binds mp_port + mp_player
 *   mp_host       127.0.0.1  joiner: the host's IPv4
 *   mp_soc_kb     256    SOC buffer when the dev link has not initialised SOC already
 *   mp_wait_ms    20000  a tick waiting longer than this for a peer = peer lost -> MP off
 *   mp_connect_s  300    the same, for the very first remote pad (the peer may boot later)
 *   mp_reopen_ms  0      while waiting, re-create the socket after this long with no datagram
 *                        from a peer that was alive (0 = never, the default)
 *   mp_stats_every 600   [mp] stats receipt cadence (ticks)
 *   mp_xform      0      joiner test lever: 1 = negate the LOCAL stick X during races, so the
 *                        two consoles' pads genuinely differ while both play one replay
 *   mp_start_tick 300    the session starts at this input tick: before it every player port is
 *                        neutral on every console and nothing blocks (stands in for the lobby)
 *   mp_hash_from  0      compare digests only from this tick on (the boot's first ~200 ticks
 *                        sample the game mid-init at a render-thread-mode-dependent phase:
 *                        renderthread=1 vs 0 differ for ONE tick there, then agree)
 *   mp_desync_at  -1     test lever: flip gRandSeed1 after tick N on THIS console (proves the
 *                        desync receipt fires)
 *   mp_fullview   1      MP phase 3: 1 = in a VS race show only this console's player, full
 *                        screen, 1P HUD (game/gdx3ds_fullview_game.c; [debug] fullview_player = k
 *                        is the same view without networking). Rendering only: the sim is unchanged
 *   mp_fullview_wide 1   mp-fullbleed: under [display] widescreen = 1 a fullview frame is the 1P
 *                        frame kind (RemoveBorders, 5:3, 1P HUD anchors) instead of the split
 *                        kind; 0 = 06c5264. Rendering only, not part of the sim-ABI word
 *
 * mp-uds keys ([net]):
 *   mp_transport  udp    udp | uds | auto. uds = 3DS local wireless (gdx3ds_mp_uds.h: no access
 *                        point, the host creates the network, joiners find it by its beacon);
 *                        auto = uds unless mp_host is set or local wireless is unavailable.
 *                        uds + wireless switch off = `[mp] init REFUSED: transport uds: Wireless
 *                        is off...` (auto falls back to udp and says so).
 *   mp_room       ""     uds joiner: join only a host whose 3DS profile name is this ("" = the
 *                        first compatible room). A uds joiner takes mp_players / mp_delay /
 *                        mp_hash_every FROM THE ROOM'S BEACON (set them on the host only), and its
 *                        player index from its node (mp_player overrides).
 *   mp_uds_probe_s 0     uds: before the session starts, measure frame-size limits and throughput
 *                        for N seconds (`[uds] probe ...` receipts; the game waits meanwhile).
 *   mp_drop       cpu    cpu | retire | coast | off. A player that leaves (LEAVE on HOME/close, the UDS
 *                        node left, or mp_drop_ms of silence -- the host decides) is removed at an
 *                        agreed tick T on every console (port/gdx_mp_drop.h): neutral pad from T;
 *                        with cpu the CPU driver drives its machine from T (MP30), with retire
 *                        its machine retires at the first race tick >= T. off =
 *                        the spike's behaviour (a lost peer ends MP on that console).
 *   mp_drop_ms    3000   host: a joiner silent this long is removed (guest time on the emulator).
 *   mp_leave_at   -1     test lever: at lockstep tick N this console says LEAVE and plays alone.
 * During a session sleep is disallowed (aptSetSleepAllowed(false)); HOME / closing the app sends
 * LEAVE (a joiner is removed; the host ends the session for everyone). Receipts: `[mp] drop:`,
 * `[mp] DROP player p removed at tick=T`, `[mp] retired player p's machine`, `[uds] ...`.
 *
 * TIME on the emulator lane: every *_ms / *_s value is GUEST time (osGetTime). Azahar
 * fast-forwards a guest whose threads all sleep, so while one instance waits for the other
 * its clock runs ~50x faster than the wall (measured: 5 guest-s of [mp] waiting = 0.1 host-s
 * in the core's own log). Emulator runs therefore need mp_wait_ms / mp_connect_s far larger
 * than hardware does, and guest-measured RTTs there are not network latency.
 *
 * Ports: player i's pad drives N64 port i; ports >= mp_players that [debug] pads reports
 * connected mirror player 0 (the race-4p-vs replay drives 4 ports from 2 consoles).
 *
 * Receipts: `[mp] init ...`, `[mp] peer <p> joined ...`, `[mp] stats tick= ...` every
 * mp_stats_every ticks, `[mp] DESYNC tick= peer= local= remote= mode=` once, `[mp] lost ...`. */
#ifndef GDX3DS_MP_H
#define GDX3DS_MP_H

#ifdef __cplusplus
extern "C" {
#endif

/* After gdx3ds_net_init (so SOC can be shared) and the config load. No-op when off. */
void gdx3ds_mp_init(void);

/* Main loop, after the game update of the tick whose pads the last poll applied. */
void gdx3ds_mp_after_tick(void);

/* 1 while a session is live. */
int gdx3ds_mp_active(void);

/* This console's player index (host 0, joiner mp_player) once a session opened, -1 when MP is
 * off or init was refused. Sticky after `[mp] lost`: the race goes on locally. MP phase 3's
 * `[net] mp_fullview = 1` shows exactly this player full screen (game/gdx3ds_fullview_game.c). */
int gdx3ds_mp_local_player(void);

/* The poll hook (port/input_bridge.c calls it weakly, after the injector and the [debug] pads
 * mirror). Returns 1 when MP rewrote the ports for this tick. Blocks (main thread) until every
 * player's pad for `tick` is in. */
int gdx_mp_exchange(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                    unsigned char* connected, int capacity);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_H */
