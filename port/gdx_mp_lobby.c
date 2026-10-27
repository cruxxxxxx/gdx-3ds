/* port/gdx_mp_lobby.c — online multiplayer lobby state machine. Contract: gdx_mp_lobby.h. */
#include "gdx_mp_lobby.h"

#include <string.h>

static void CopyStr(char* dst, const char* src, int cap) {
    int i;
    for (i = 0; i < cap - 1 && src != NULL && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void gdx_lobby_init(GdxMpLobby* lb, int mySlot, uint64_t secret, const char* build, uint32_t contentHash,
                    const char* name) {
    memset(lb, 0, sizeof(*lb));
    lb->mySlot = mySlot;
    lb->secret = secret;
    CopyStr(lb->build, build, sizeof(lb->build));
    lb->contentHash = contentHash;
    gdx_mpo_name_sanitise(name, lb->name);
    lb->engine = 50;
    lb->engineBits = gdx_mpo_engine_bits(0.5f);
    lb->state = GDX_LB_STATE_LOBBY;
    lb->abortFrom = -1;
    lb->pickMsLeft = GDX_LB_NO_ROUND;
    gdx_lobby_set_course(lb, 0, gdx_mpo_stock_course_key(0));
}

void gdx_lobby_set_course(GdxMpLobby* lb, int course, uint32_t key) {
    lb->course = (course >= 0 && course <= GDX_MPO_MAX_COURSE) ? course : 0;
    lb->courseKey = key;
}

int gdx_lobby_host_course(const GdxMpLobby* lb) {
    const GdxMpLobbyPeer* host;
    if (lb->mySlot == 0) {
        return lb->course;
    }
    host = &lb->peer[0];
    if (!host->present || !host->heard || !host->versionOk) {
        return -1;
    }
    return host->hello.courseFor;
}

static const GdxMpLobbyPeer* HeardHost(const GdxMpLobby* lb) {
    const GdxMpLobbyPeer* host = &lb->peer[0];
    return (host->present && host->heard && host->versionOk) ? host : NULL;
}

int gdx_lobby_host_racing(const GdxMpLobby* lb) {
    const GdxMpLobbyPeer* host;
    if (lb->mySlot == 0) {
        return 0;
    }
    host = HeardHost(lb);
    return host != NULL && host->hello.state != GDX_LB_STATE_LOBBY;
}

int gdx_lobby_host_round(const GdxMpLobby* lb) {
    const GdxMpLobbyPeer* host;
    if (lb->mySlot == 0) {
        return lb->pickRound;
    }
    host = HeardHost(lb);
    return host != NULL ? host->hello.pickRound : 0;
}

uint32_t gdx_lobby_host_ms_left(const GdxMpLobby* lb) {
    const GdxMpLobbyPeer* host;
    if (lb->mySlot == 0) {
        return lb->pickMsLeft;
    }
    host = HeardHost(lb);
    return host != NULL ? host->hello.pickMsLeft : GDX_LB_NO_ROUND;
}

int gdx_lobby_peer_picked(const GdxMpLobby* lb, int slot) {
    const GdxMpLobbyPeer* p;
    if (slot == lb->mySlot) {
        return lb->picked && lb->pickRound != 0 && lb->pickRound == (uint8_t) gdx_lobby_host_round(lb);
    }
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS) {
        return 0;
    }
    p = &lb->peer[slot];
    return p->present && p->heard && p->hello.picked && p->hello.pickRound != 0 &&
           p->hello.pickRound == (uint8_t) gdx_lobby_host_round(lb);
}

int gdx_lobby_picks_in(const GdxMpLobby* lb) {
    int s;
    if (!gdx_lobby_peer_picked(lb, lb->mySlot)) {
        return 0;
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        if (s != lb->mySlot && lb->peer[s].present && !gdx_lobby_peer_picked(lb, s)) {
            return 0;
        }
    }
    return 1;
}

int gdx_lobby_host_picking(const GdxMpLobby* lb) {
    const GdxMpLobbyPeer* host;
    if (lb->mySlot == 0) {
        return lb->picking;
    }
    host = &lb->peer[0];
    return (host->present && host->heard) ? host->hello.picking : GDX_LB_PICK_NONE;
}

void gdx_lobby_set_roster(GdxMpLobby* lb, const GdxMpRvRoster* roster) {
    int s;
    if (lb->mySlot >= 0 && lb->mySlot < GDX_MPO_MAX_SLOTS && roster->member[lb->mySlot].present) {
        lb->myEpoch = roster->member[lb->mySlot].epoch;
    }
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        GdxMpLobbyPeer* p = &lb->peer[s];
        const int present = roster->member[s].present && s != lb->mySlot;
        if (!present) {
            if (p->present) {
                memset(p, 0, sizeof(*p));
            }
            continue;
        }
        if (!p->present || p->epoch != roster->member[s].epoch) {
            /* New, or a different console in the slot (a rejoin after a restart): nothing we
             * heard from the old one (ready, version, RTT) carries over. */
            memset(p, 0, sizeof(*p));
            p->present = 1;
            p->epoch = roster->member[s].epoch;
        }
        CopyStr(p->name, roster->member[s].name, sizeof(p->name));
    }
}

static void FillHeader(GdxMpLobby* lb, GdxMpLbHeader* h, int type, int dst, uint32_t nowMs) {
    const GdxMpLobbyPeer* p = &lb->peer[dst];
    memset(h, 0, sizeof(*h));
    h->type = (uint8_t) type;
    h->from = (uint8_t) lb->mySlot;
    h->to = (uint8_t) dst;
    h->secret = lb->secret;
    h->sendMs = nowMs;
    if (p->haveTheirSend) {
        const uint32_t held = nowMs - p->theirSendRxMs;
        h->echoMs = p->theirSendMs;
        h->holdMs = (uint16_t) (held > 0xFFFEu ? 0xFFFEu : held);
    } else {
        h->echoMs = 0;
        h->holdMs = 0xFFFFu; /* nothing to echo */
    }
}

static int DstOk(const GdxMpLobby* lb, int dst) {
    return dst >= 0 && dst < GDX_MPO_MAX_SLOTS && dst != lb->mySlot && lb->peer[dst].present;
}

int gdx_lobby_build_hello(GdxMpLobby* lb, int dst, uint32_t nowMs, int directMask, uint8_t* out, int cap) {
    GdxMpLbHeader h;
    GdxMpLbHello b;
    int s;
    if (!DstOk(lb, dst)) {
        return 0;
    }
    FillHeader(lb, &h, GDX_LB_HELLO, dst, nowMs);
    memset(&b, 0, sizeof(b));
    b.proto = GDX_MPO_PROTO;
    b.state = (uint8_t) lb->state;
    b.ready = (uint8_t) (lb->ready ? 1 : 0);
    b.contentHash = lb->contentHash;
    b.startGen = lb->startGen;
    b.machine = (uint8_t) lb->machine;
    b.skin = (uint8_t) lb->skin;
    b.engine = (uint8_t) lb->engine;
    b.bot = (uint8_t) (lb->bot >= 2 ? 2 : (lb->bot ? 1 : 0)); /* 2 = ram mode (MP30) */
    b.directMask = (uint8_t) directMask;
    b.removedMask = lb->removedMask;
    b.model = lb->model;
    for (s = 0; s < GDX_MPO_MAX_SLOTS; s++) {
        const uint32_t r = (s == lb->mySlot) ? 0u : gdx_lobby_peer_rtt(lb, s);
        b.rttMs[s] = (uint16_t) (r > 0xFFFFu ? 0xFFFFu : r);
    }
    CopyStr(b.build, lb->build, sizeof(b.build));
    CopyStr(b.name, lb->name, sizeof(b.name));
    b.simAbi = lb->simAbi;
    b.picking = (uint8_t) (lb->picking >= GDX_LB_PICK_NONE && lb->picking <= GDX_LB_PICK_COURSE ? lb->picking : 0);
    b.pickRound = lb->pickRound;
    b.picked = (uint8_t) (lb->picked ? 1 : 0);
    b.pickMsLeft = (lb->pickMsLeft <= GDX_MPO_PICK_MS) ? lb->pickMsLeft : GDX_LB_NO_ROUND;
    b.courseFor = (uint8_t) lb->course;
    b.courseKey = lb->courseKey;
    b.engineBits = gdx_mpo_engine_bits(gdx_mpo_engine_from_bits(lb->engineBits));
    return gdx_lb_build(&h, &b, out, cap);
}

/* The START's player index for slot `slot`, -1 if it names no player there. */
static int StartPlayerOfSlot(const GdxMpLobby* lb, int slot) {
    int i;
    for (i = 0; i < lb->start.numPlayers; i++) {
        if (lb->start.player[i].slot == slot) {
            return i;
        }
    }
    return -1;
}

int gdx_lobby_start_player_here(const GdxMpLobby* lb, int p) {
    int slot;
    if (!lb->haveStart || p < 0 || p >= lb->start.numPlayers) {
        return 0;
    }
    slot = lb->start.player[p].slot;
    if (slot == lb->mySlot) {
        return 1;
    }
    return lb->peer[slot].present && lb->peer[slot].epoch == lb->start.player[p].epoch;
}

int gdx_lobby_build_start(GdxMpLobby* lb, int dst, uint32_t nowMs, uint8_t* out, int cap) {
    GdxMpLbHeader h;
    const int p = (dst >= 0 && dst < GDX_MPO_MAX_SLOTS) ? StartPlayerOfSlot(lb, dst) : -1;
    if (!DstOk(lb, dst) || !lb->haveStart || lb->mySlot != 0) {
        return 0;
    }
    if (p < 0 || !gdx_lobby_start_player_here(lb, p)) {
        return 0; /* not in this START, or its slot holds another console now (a rejoin): never theirs */
    }
    FillHeader(lb, &h, GDX_LB_START, dst, nowMs);
    return gdx_lb_build(&h, &lb->start, out, cap);
}

int gdx_lobby_build_abort(GdxMpLobby* lb, int dst, uint32_t nowMs, const GdxMpLbAbort* a, uint8_t* out, int cap) {
    GdxMpLbHeader h;
    if (!DstOk(lb, dst) || a == NULL) {
        return 0;
    }
    FillHeader(lb, &h, GDX_LB_ABORT, dst, nowMs);
    return gdx_lb_build(&h, a, out, cap);
}

static void NoteRtt(GdxMpLobbyPeer* p, const GdxMpLbHeader* h, uint32_t nowMs) {
    uint32_t rtt;
    if (h->holdMs == 0xFFFFu) {
        return; /* the sender had nothing of ours to echo yet */
    }
    rtt = nowMs - h->echoMs;
    if (rtt > 60000u) {
        return; /* an echo of something we never sent this session (or a clock wrap) */
    }
    rtt = (rtt > h->holdMs) ? rtt - h->holdMs : 0;
    p->rttMs = rtt;
    p->win[p->winPos] = (uint16_t) (rtt > 0xFFFEu ? 0xFFFEu : rtt);
    p->winPos = (p->winPos + 1) % GDX_LOBBY_RTT_WINDOW;
    if (p->winCount < GDX_LOBBY_RTT_WINDOW) {
        p->winCount++;
    }
    p->samples++;
}

int gdx_lobby_ingest(GdxMpLobby* lb, const uint8_t* pkt, int len, uint32_t nowMs, int* fromOut) {
    GdxMpLbHeader h;
    GdxMpLbBody b;
    GdxMpLobbyPeer* p;
    const int type = gdx_lb_parse(pkt, len, lb->secret, &h, &b);
    if (fromOut != NULL) {
        *fromOut = -1;
    }
    if (type < 0) {
        lb->rejected++;
        return type;
    }
    if (h.from == lb->mySlot || !lb->peer[h.from].present || (h.to != lb->mySlot && h.to != 0xFF)) {
        lb->rejected++;
        return -4;
    }
    if (fromOut != NULL) {
        *fromOut = h.from;
    }
    p = &lb->peer[h.from];
    /* Every type carries the ping fields. sendMs only moves forward (a replayed old packet
     * must not rewind the echo we hand back). */
    if (!p->haveTheirSend || (int32_t) (h.sendMs - p->theirSendMs) > 0) {
        p->theirSendMs = h.sendMs;
        p->theirSendRxMs = nowMs;
        p->haveTheirSend = 1;
    }
    NoteRtt(p, &h, nowMs);
    if (type == GDX_LB_HELLO) {
        p->hello = b.hello;
        p->heard = 1;
        p->lastHelloMs = nowMs;
        p->versionOk = (b.hello.proto == GDX_MPO_PROTO) && strcmp(b.hello.build, lb->build) == 0 &&
                       b.hello.contentHash == lb->contentHash && b.hello.simAbi == lb->simAbi;
        if (!p->versionOk) {
            lb->versionRejected++;
            if (b.hello.proto == GDX_MPO_PROTO && b.hello.simAbi != lb->simAbi) {
                lb->abiRejected++;
            }
        }
        return type;
    }
    if (type == GDX_LB_START) {
        int i;
        int listed = 0;
        if (lb->mySlot == 0 || h.from != 0) {
            lb->rejected++;
            return -4;
        }
        if (lb->haveStart && (int32_t) (b.start.gen - lb->startGen) <= 0) {
            return type; /* a repeat of what we already hold */
        }
        for (i = 0; i < b.start.numPlayers; i++) {
            if (b.start.player[i].slot == lb->mySlot && b.start.player[i].epoch == lb->myEpoch) {
                listed = 1; /* our slot AND this console: a START for the slot's previous one is not ours */
            }
        }
        if (b.start.simAbi != lb->simAbi) {
            lb->abiRejected++;
            lb->rejected++;
            return -5; /* another flavour of the game: never race it */
        }
        if (listed && (b.start.course != lb->course || b.start.courseKey != lb->courseKey ||
                       lb->courseKey == GDX_MPO_NO_COURSE_KEY)) {
            /* stockpick: not the course this console holds (or not one it holds at all): racing it
             * would desync at the first tick. Not acknowledged: the host's launch times out. */
            lb->courseRejected++;
            lb->rejected++;
            return -6;
        }
        lb->startGen = b.start.gen; /* acknowledge even one we are not in */
        if (!listed) {
            return type;
        }
        lb->start = b.start;
        lb->haveStart = 1;
        lb->startIsNew = 1;
        return type;
    }
    if (type == GDX_LB_RM_POLL || type == GDX_LB_RM_HAVE || type == GDX_LB_RM_SET) {
        lb->rm = b.remove; /* the session acts on it at once (gdx_mp_remove.h) */
        return type;
    }
    /* ABORT */
    lb->abort = b.abort;
    lb->abortFrom = h.from;
    lb->abortIsNew = 1;
    return type;
}

int gdx_lobby_peer_live(const GdxMpLobby* lb, int slot, uint32_t nowMs) {
    const GdxMpLobbyPeer* p;
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS || slot == lb->mySlot) {
        return 0;
    }
    p = &lb->peer[slot];
    return p->present && p->heard && p->versionOk && (int32_t) (nowMs - p->lastHelloMs) <= GDX_LOBBY_STALE_MS;
}

uint32_t gdx_lobby_peer_rtt(const GdxMpLobby* lb, int slot) {
    const GdxMpLobbyPeer* p = &lb->peer[slot];
    uint32_t m = 0;
    int i;
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS || p->winCount == 0) {
        return GDX_LB_NO_RTT;
    }
    for (i = 0; i < p->winCount; i++) {
        if (p->win[i] > m) {
            m = p->win[i];
        }
    }
    return m;
}

uint32_t gdx_lobby_peer_jitter(const GdxMpLobby* lb, int slot) {
    const GdxMpLobbyPeer* p = &lb->peer[slot];
    uint32_t lo = 0xFFFFu;
    uint32_t hi = 0;
    int i;
    if (slot < 0 || slot >= GDX_MPO_MAX_SLOTS || p->winCount == 0) {
        return 0;
    }
    for (i = 0; i < p->winCount; i++) {
        if (p->win[i] < lo) {
            lo = p->win[i];
        }
        if (p->win[i] > hi) {
            hi = p->win[i];
        }
    }
    return (hi - lo) / 2u;
}

/* stockpick: with requirePicks, a player is startable once it reported its pick for the round --
 * or once the host stopped waiting for it (pickWaitOver: gdx_lobby_host_make_start then gives a
 * player with no pick the default machine). */
/* A peer whose proto / build / content differs: which side should update (the proto says; the
 * same proto with another build or flavour cannot say). */
static const char* VersionWhyNot(const GdxMpLobby* lb, const GdxMpLobbyPeer* p) {
    if (p->hello.proto == GDX_MPO_PROTO && p->hello.simAbi != lb->simAbi) {
        return "a player runs the other edition (EK / shipped)";
    } else if (p->hello.proto > GDX_MPO_PROTO) {
        return "a player runs a newer version: update this console";
    } else if (p->hello.proto < GDX_MPO_PROTO) {
        return "a player runs an older version: they need an update";
    }
    return "a player runs another build of this version: everyone needs the same one";
}

static int PeerPickOk(const GdxMpLobby* lb, int slot, const char** whyNot) {
    if (!lb->requirePicks || lb->pickWaitOver || gdx_lobby_peer_picked(lb, slot)) {
        return 1;
    }
    *whyNot = "waiting for a player to pick a machine";
    return 0;
}

/* stockpick: every player must report the very course the host picked, with the host's key for it
 * (the same course data on every console). */
static int PeerCourseOk(const GdxMpLobby* lb, const GdxMpLobbyPeer* p, const char** whyNot) {
    if (p->hello.courseFor != (uint8_t) lb->course) {
        *whyNot = "checking the course on every console";
        return 0;
    }
    if (p->hello.courseKey != lb->courseKey) {
        *whyNot = "a player does not have the course";
        return 0;
    }
    return 1;
}

/* The slots a START would take now, ascending (the host first). Returns the count, or 0 and
 * *whyNot when some present member is not ready/live/matched. */
static int StartSlots(const GdxMpLobby* lb, uint32_t nowMs, int* slots, const char** whyNot) {
    int n = 0;
    int s;
    const char* dummy;
    if (whyNot == NULL) {
        whyNot = &dummy;
    }
    *whyNot = "";
    if (lb->mySlot != 0) {
        *whyNot = "only the host starts";
        return 0;
    }
    if (lb->picking == GDX_LB_PICK_COURSE) {
        *whyNot = "the host is picking a course";
        return 0;
    }
    if (lb->picking != GDX_LB_PICK_NONE) {
        *whyNot = "the host is picking a machine";
        return 0;
    }
    if (lb->requirePicks && lb->pickRound == 0) {
        *whyNot = "open machine select first";
        return 0;
    }
    if (lb->requirePicks && !lb->picked) {
        *whyNot = "pick your machine first";
        return 0;
    }
    if (!gdx_mpo_course_raceable(lb->course)) {
        *whyNot = "that course cannot be raced online";
        return 0;
    }
    if (lb->courseKey == GDX_MPO_NO_COURSE_KEY) {
        *whyNot = "this console does not have the course";
        return 0;
    }
    slots[n++] = 0;
    for (s = 1; s < GDX_MPO_MAX_SLOTS; s++) {
        const GdxMpLobbyPeer* p = &lb->peer[s];
        if (!p->present) {
            continue;
        }
        if (!p->heard || (int32_t) (nowMs - p->lastHelloMs) > GDX_LOBBY_STALE_MS) {
            if (lb->skipAway && p->heard) {
                continue; /* START WITHOUT: the quiet member stays in the room, out of this race */
            }
            *whyNot = "waiting for a player to connect";
            return 0;
        }
        if (!p->versionOk) {
            *whyNot = VersionWhyNot(lb, p);
            return 0;
        }
        if (!p->hello.ready && !lb->requirePicks) {
            *whyNot = "waiting for everyone to be ready";
            return 0;
        }
        if (!PeerPickOk(lb, s, whyNot) || !PeerCourseOk(lb, p, whyNot)) {
            return 0;
        }
        slots[n++] = s;
    }
    if (n < 2) {
        *whyNot = "need at least 2 players";
        return 0;
    }
    return n;
}

int gdx_lobby_host_startable(const GdxMpLobby* lb, uint32_t nowMs, const char** whyNot) {
    int slots[GDX_MPO_MAX_SLOTS];
    return StartSlots(lb, nowMs, slots, whyNot);
}

static int PeerAway(const GdxMpLobbyPeer* p, uint32_t nowMs) {
    return p->present && p->heard && (int32_t) (nowMs - p->lastHelloMs) > GDX_LOBBY_STALE_MS;
}

int gdx_lobby_host_startable_without_away(const GdxMpLobby* lb, uint32_t nowMs, int* firstAwaySlot) {
    GdxMpLobby skip;
    int slots[GDX_MPO_MAX_SLOTS];
    int s;
    int n;
    if (firstAwaySlot != NULL) {
        *firstAwaySlot = -1;
    }
    if (lb->mySlot != 0 || StartSlots(lb, nowMs, slots, NULL) > 0) {
        return 0;
    }
    skip = *lb;
    skip.skipAway = 1;
    n = StartSlots(&skip, nowMs, slots, NULL);
    if (n < 2) {
        return 0;
    }
    for (s = 1; s < GDX_MPO_MAX_SLOTS; s++) {
        if (PeerAway(&lb->peer[s], nowMs)) {
            if (firstAwaySlot != NULL) {
                *firstAwaySlot = s;
            }
            return n;
        }
    }
    return 0;
}

static uint32_t PairRtt(const GdxMpLobby* lb, int a, int b, uint32_t* jitter) {
    /* Our own measurement where we are one end; otherwise the worse of the two ends' reports. */
    uint32_t r = GDX_LB_NO_RTT;
    *jitter = 0;
    if (a == lb->mySlot || b == lb->mySlot) {
        const int other = (a == lb->mySlot) ? b : a;
        r = gdx_lobby_peer_rtt(lb, other);
        *jitter = gdx_lobby_peer_jitter(lb, other);
        return r;
    }
    {
        const uint32_t ab = lb->peer[a].hello.rttMs[b];
        const uint32_t ba = lb->peer[b].hello.rttMs[a];
        if (ab != GDX_LB_NO_RTT && ba != GDX_LB_NO_RTT) {
            r = ab > ba ? ab : ba;
        } else if (ab != GDX_LB_NO_RTT) {
            r = ab;
        } else {
            r = ba;
        }
    }
    return r;
}

/* Extra input delay when an Old 3DS races (lb->o3dsExtraDelay, [net] mp_o3ds_extra_delay).
 * d07ae82 used +3: on hardware (90049af) the Old 3DS rendered every 4th tick and then held the
 * present ~24 ms, so it sent a pad ~38 times a second and both consoles stalled. That is a RATE
 * the delay cannot fix (tools/mp-tickmodel: 38.8-39.3 ticks/s at every delay 2..7). With the
 * lockstep-aware frameskip and the Old 3DS IPC cuts the model holds ~58 ticks/s and its stall
 * minimum already at the RTT delay (4 on local wireless), so the default is +0; the lever keeps
 * a margin one ini edit away. */
static int FrameGapTicks(const GdxMpLobby* lb, const int* slots, int n) {
    int i;
    if (lb->o3dsExtraDelay <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        const uint8_t model = (slots[i] == lb->mySlot) ? lb->model : lb->peer[slots[i]].hello.model;
        if (model == GDX_MPO_MODEL_O3DS) {
            return lb->o3dsExtraDelay;
        }
    }
    return 0;
}

int gdx_lobby_pick_delay(const GdxMpLobby* lb, uint32_t nowMs, uint32_t* worstRtt, uint32_t* worstJitter) {
    int slots[GDX_MPO_MAX_SLOTS];
    const int n = StartSlots(lb, nowMs, slots, NULL);
    uint32_t wr = 0;
    uint32_t wj = 0;
    int unknown = 0;
    int i;
    int j;
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            uint32_t jit;
            const uint32_t r = PairRtt(lb, slots[i], slots[j], &jit);
            if (r == GDX_LB_NO_RTT) {
                unknown = 1;
                continue;
            }
            if (r > wr) {
                wr = r;
            }
            if (jit > wj) {
                wj = jit;
            }
        }
    }
    if (worstRtt != NULL) {
        *worstRtt = wr;
    }
    if (worstJitter != NULL) {
        *worstJitter = wj;
    }
    if (unknown && wr < 100) {
        wr = 100; /* a pair we have no number for yet: assume a typical internet path */
    }
    {
        int d = gdx_mpo_delay_from_rtt(wr, wj) + FrameGapTicks(lb, slots, n);
        if (d > GDX_MPO_MAX_DELAY) {
            d = GDX_MPO_MAX_DELAY;
        }
        return d;
    }
}

static void SetDefaultPick(GdxMpStartPlayer* pl) {
    pl->machine = GDX_MPO_DEFAULT_MACHINE;
    pl->skin = GDX_MPO_DEFAULT_SKIN;
    pl->engine = GDX_MPO_DEFAULT_ENGINE;
    pl->engineBits = gdx_mpo_engine_bits(0.5f);
}

int gdx_lobby_host_make_start(GdxMpLobby* lb, const GdxMpLobbySettings* s, uint32_t seed1, uint32_t mask1,
                              uint32_t seed2, uint32_t mask2, uint64_t token, uint32_t nowMs) {
    int slots[GDX_MPO_MAX_SLOTS];
    const int n = StartSlots(lb, nowMs, slots, NULL);
    GdxMpLbStart st;
    int i;
    if (n < 2 || s == NULL || !gdx_mpo_course_raceable(s->course) || s->course != lb->course || token == 0) {
        return 0; /* the settings' course must be the one every console reported on */
    }
    memset(&st, 0, sizeof(st));
    st.gen = lb->startGen + 1u;
    st.numPlayers = (uint8_t) n;
    st.course = s->course;
    st.delay = (uint8_t) ((s->delayOverride >= 1 && s->delayOverride <= GDX_MPO_MAX_DELAY)
                              ? s->delayOverride
                              : gdx_lobby_pick_delay(lb, nowMs, NULL, NULL));
    st.hashEvery = (uint8_t) (s->hashEvery >= 1 && s->hashEvery <= 60 ? s->hashEvery : 1);
    st.vsCom = (uint8_t) (s->vsCom ? 1 : 0);
    st.handicap = (uint8_t) (s->handicap <= 2 ? s->handicap : 0);
    st.vsSlot = (uint8_t) (s->vsSlot ? 1 : 0);
    st.difficulty = (uint8_t) (s->difficulty <= 3 ? s->difficulty : 0);
    st.laps = (uint8_t) (s->laps >= 1 && s->laps <= 9 ? s->laps : 3);
    if (st.vsCom && s->racers >= GDX_MPO_MIN_RACERS && s->racers <= GDX_MPO_MAX_RACERS) {
        st.racers = (uint8_t) (s->racers < n ? n : s->racers);
    }
    st.seed1 = seed1;
    st.mask1 = mask1;
    st.seed2 = seed2;
    st.mask2 = mask2;
    st.token = token;
    st.simAbi = lb->simAbi;
    st.courseKey = lb->courseKey;
    for (i = 0; i < n; i++) {
        GdxMpStartPlayer* pl = &st.player[i];
        pl->slot = (uint8_t) slots[i];
        pl->epoch = (slots[i] == lb->mySlot) ? lb->myEpoch : lb->peer[slots[i]].epoch;
        if (slots[i] == lb->mySlot) {
            pl->machine = (uint8_t) lb->machine;
            pl->skin = (uint8_t) lb->skin;
            pl->engine = (uint8_t) lb->engine;
            pl->engineBits = gdx_mpo_engine_bits(gdx_mpo_engine_from_bits(lb->engineBits));
            if (lb->requirePicks && !gdx_lobby_peer_picked(lb, lb->mySlot)) {
                SetDefaultPick(pl);
            }
            if (lb->bot) {
                st.botMask |= (uint8_t) (1u << i);
            }
            if (lb->bot >= 2) {
                st.ramMask |= (uint8_t) (1u << i);
            }
        } else {
            const GdxMpLbHello* h = &lb->peer[slots[i]].hello;
            pl->machine = h->machine;
            pl->skin = h->skin;
            pl->engine = h->engine;
            pl->engineBits = h->engineBits;
            if (lb->requirePicks && !gdx_lobby_peer_picked(lb, slots[i])) {
                SetDefaultPick(pl); /* never reported a pick for the round */
            }
            if (h->bot) {
                st.botMask |= (uint8_t) (1u << i);
            }
            if (h->bot >= 2) {
                st.ramMask |= (uint8_t) (1u << i);
            }
        }
    }
    lb->start = st;
    lb->haveStart = 1;
    lb->startGen = st.gen;
    lb->startIsNew = 1;
    return n;
}

int gdx_lobby_start_acked(const GdxMpLobby* lb) {
    int i;
    if (!lb->haveStart) {
        return 0;
    }
    for (i = 0; i < lb->start.numPlayers; i++) {
        const int s = lb->start.player[i].slot;
        if (s == lb->mySlot || !gdx_lobby_start_player_here(lb, i) || ((lb->removedMask >> i) & 1u)) {
            continue;
        }
        if (!lb->peer[s].heard || (int32_t) (lb->peer[s].hello.startGen - lb->start.gen) < 0) {
            return 0;
        }
    }
    return 1;
}

int gdx_lobby_my_player(const GdxMpLobby* lb) {
    int i;
    if (!lb->haveStart) {
        return -1;
    }
    for (i = 0; i < lb->start.numPlayers; i++) {
        if (lb->start.player[i].slot == lb->mySlot) {
            return i;
        }
    }
    return -1;
}

int gdx_lobby_player_slot(const GdxMpLobby* lb, int p) {
    if (!lb->haveStart || p < 0 || p >= lb->start.numPlayers) {
        return -1;
    }
    return lb->start.player[p].slot;
}
