/* port/3ds/net/gdx3ds_mp_radio.h — what an online / LAN room asks of the rest of the console while
 * it is up (research: artifacts/mp-netcode-research/network.md, rank 1).
 *
 * Hardware online1: during a 15 s silence the Old 3DS's keepalive to the server took 15 s to come
 * back too, so its whole radio path stalled, not just the peer link. The app never asked NDM for
 * the radio, so the background daemons (StreetPass CEC, SpotPass BOSS, NIM) may take it, and the
 * dev [net] thread retried a dead log host all session on the one soc:U session. While a room is
 * up this module:
 *   - [net] mp_ndm_exclusive (default 1; infrastructure rooms only, never a local wireless one):
 *     NDMU_EnterExclusiveState(INFRASTRUCTURE) + NDMU_SuspendDaemons(background), undone when the
 *     room goes, at exit and across HOME / sleep (gdx3ds_mp_radio_release); card-3: 2 = the
 *     per-race A/B (port/gdx_mp_netab.h): held in the arm-A races, left at the START of an arm-B
 *     race ("ndm leave (A/B arm B ...)"), taken again at the next arm-A START;
 *   - quiets the dev net thread (gdx3ds_net_set_mp_quiet, [net] mp_net_quiet);
 *   - keeps the log's MP reserve on (gdx3ds_filelog_set_mp_session).
 * Receipts: "[mpradio] ndm enter ..." / "ndm leave (<why>) ..." with the NDM state and the CEC /
 * BOSS daemon status before and after. Main thread only. */
#ifndef GDX3DS_MP_RADIO_H
#define GDX3DS_MP_RADIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Every frame (cheap: acts only on a change). inRoom: a room or race is up; infra: it runs over
 * WiFi infrastructure (online / LAN), not local wireless. */
void gdx3ds_mp_radio_update(int inRoom, int infra);
/* Undo everything now (HOME / sleep / exit); the next update with a room up re-applies it. */
void gdx3ds_mp_radio_release(const char* why);
/* WiFi bars from the shared page (0-3, no IPC). */
int gdx3ds_mp_radio_bars(void);
/* 1 = NDM exclusive state is held. */
int gdx3ds_mp_radio_exclusive(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MP_RADIO_H */
