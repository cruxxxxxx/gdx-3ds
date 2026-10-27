/* port/3ds/gfx/n64/gdx_nicensus.cpp -- [debug] nicensus: what the owned walker still interprets,
 * per game mode, per context. docs/research/native/nicensus-progress.md.
 *
 * MEASUREMENT ONLY. Every function here reads the command words, the interpreter's counters and
 * the RDP/RSP registers, and writes nothing but its own NcState. With the lever off mNc is null:
 * Walk() pays one pointer test per task (the counting loop is the separate WalkNicensus body) and
 * the cold Exec sites (HUD replay, nested lists, the seam's hook commands) one test per command.
 *
 * Attribution. A rendered frame is one task (renderthread=0, the Old 3DS execution). The glue
 * (gdx_rt_glue.cpp) opens it in a bucket = gGameMode * kNcSubs + race sub-scope, and switches the
 * context around the marker hooks (NcSwitch). Every clock tick and counter delta between two
 * switches belongs to the context that was current, so the contexts are SELF time and add up to
 * the task. At the frame's end the walker's own share is filed as "between NRR markers" (walkB) when
 * the frame dispatched an NRR marker, else as "whole screen, no NRR" (walkW). */
#include "gdx_interp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gdx_log.h"

extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

namespace {

constexpr int kNcBuckets = Interp::kNcModes * Interp::kNcSubs;
constexpr int kNcSlots = Interp::NC_COUNT + 1; /* walkB, walkW, then contexts 1..6 */
constexpr int kNcNrrCats = 9;
constexpr int kCombSet = 128;

struct NcAcc {
    uint64_t ticks;
    uint32_t cmds;   /* commands through the instrumented Exec sites */
    uint32_t words;  /* words consumed without Exec (NRR HUD decoded, retained HUD runs) */
    uint32_t wordsRetained;
    uint32_t tris, emit, rects, loads, imports, draws, vtx;
    uint32_t enters;
};

struct NcStatSnap {
    uint32_t tris, emit, rects, loads, imports, draws, vtx;
};

struct NcBucket {
    uint32_t frames, lus, nrrFrames;
    uint64_t taskTicks;
    NcAcc acc[kNcSlots];
    uint32_t ops[kNcSlots][256];
    uint32_t nrrMk[kNcNrrCats], nrrDr[kNcNrrCats], nrrVx[kNcNrrCats];
    /* features, sampled at every interpreted draw command (TRI1/TRI2/QUAD/LINE3D/TEXRECT/FILLRECT) */
    uint32_t drawCmds;
    uint32_t cycle[4];      /* 1CYC 2CYC COPY FILL */
    uint32_t filt[4];       /* POINT, -, BILERP, AVERAGE (other_mode_h bits 12-13) */
    uint32_t tlut[4];       /* NONE, -, RGBA16, IA16 (bits 14-15) */
    uint32_t zcu[4];        /* bit0 Z_CMP, bit1 Z_UPD */
    uint32_t zmode[4];      /* OPA INTER XLU DEC */
    uint32_t acmp[4];       /* alpha compare NONE THRESHOLD -, DITHER */
    uint32_t geoLight, geoFog, geoTexGen, geoCullF, geoCullB, geoZbuf, geoShade, geoSmooth;
    uint64_t comb[kCombSet];
    uint8_t combUsed[kCombSet];
    uint32_t combDistinct, combOverflow;
    /* command-decoded features */
    uint32_t moveword[16];  /* index / 2: MATRIX NUMLIGHT CLIP SEGMENT FOG LIGHTCOL FORCEMTX PERSPNORM */
    uint32_t movemem[16];   /* index & 15: 8 VIEWPORT, 10 LIGHT, 14 MATRIX, ... */
    uint32_t tileFmt[32];   /* SETTILE fmt*4 + siz */
    uint32_t timgFmt[32];   /* SETTIMG fmt*4 + siz */
    uint32_t f3dCmds;       /* commands from legacy-F3D lists (remapped) */
    uint32_t variant[8];    /* frames by the task's ucode variant at its end */
    uint32_t wipeType[16];  /* menus-s4 nicensus_wipe: frames by the transition type drawn */
};

/* [debug] ni_strict: one (context, site, tag, opcode) cell of a bucket, with its persistence. */
struct NsEntry {
    uint32_t key;    /* ctx << 24 | site << 16 | tag << 8 | op */
    uint32_t n;      /* commands */
    uint32_t frames; /* frames it appeared in */
    uint32_t visits; /* bucket visits (consecutive runs of frames) it appeared in */
    uint32_t maxF;   /* most in one frame */
    uint32_t curF;   /* this frame's count (folded into maxF when the next frame touches it) */
    uint32_t seq;    /* the frame sequence number of curF */
    uint32_t lastVisit;
    uint32_t firstF, lastF; /* bucket-local frame index at the first / last appearance */
    uint8_t used;
};

constexpr int kNsSlots = 1024;

struct NsTable {
    uint32_t frames, visits;
    uint64_t strict, disp, direct;    /* per-class command totals */
    uint32_t maxStrict, zeroStrict;  /* most strict commands in one frame; frames with none */
    uint32_t hist[5];                /* frames by strict count: 0, 1-9, 10-99, 100-999, 1000+ */
    uint32_t used, overflow;
    NsEntry e[kNsSlots];
};

const char* const kModeName[Interp::kNcModes] = {
    "title",      "gp_race",     "practice",    "vs2p",       "vs3p",          "vs4p",
    "records",    "main_menu",   "mach_select", "mach_settings", "course_select", "credits_skip",
    "credits",    "course_edit", "time_attack", "gp_next_course", "create_machine", "gp_end_cs",
    "gp_next_mach", "records_course", "options", "death_race", "ead_demo",  "other"
};
const char* const kSubName[Interp::kNcSubs] = { "steady", "intro", "racing", "post", "change", "demo", "wipe" };
const char* const kSlotName[kNcSlots] = { "walkB", "walkW", "nrr", "nrrhud", "hud", "seam", "mesh", "frame" };
const char* const kNrrCatName[kNcNrrCats] = { "hud", "trk", "mach", "sky", "dress", "shd", "fx", "self", "deco" };
const char* const kNsSiteName[Interp::NCS_COUNT] = { "walk",   "nested", "niex",   "nistx",  "niseg", "nirecov",
                                                     "hudrun", "hudirect", "hudplain", "hookcmd", "course" };
const char* const kNsCtxName[Interp::NC_COUNT] = { "walk", "nrr", "nrrhud", "hud", "seam", "mesh", "frame" };

/* One output line, flushed into continuation lines before gdxn_logf's 320-byte buffer. */
class NcLine {
  public:
    NcLine(const char* head) {
        snprintf(mHead, sizeof(mHead), "%s", head);
        Start(false);
    }
    ~NcLine() {
        Flush();
    }
    void Add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char tok[160];
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(tok, sizeof(tok), fmt, ap);
        va_end(ap);
        if (n <= 0) {
            return;
        }
        if (mLen + n > kMax) {
            Flush();
            Start(true);
        }
        memcpy(mBuf + mLen, tok, static_cast<size_t>(n) < sizeof(tok) ? static_cast<size_t>(n) : sizeof(tok) - 1);
        mLen += (n < static_cast<int>(sizeof(tok))) ? n : static_cast<int>(sizeof(tok)) - 1;
        mBuf[mLen] = 0;
    }

  private:
    static constexpr int kMax = 290;
    void Start(bool cont) {
        mLen = snprintf(mBuf, sizeof(mBuf), "%s%s", mHead, cont ? " +" : "");
    }
    void Flush() {
        if (mLen > 0) {
            gdxn_logf("%s", mBuf);
        }
        mLen = 0;
    }
    char mHead[96];
    char mBuf[kMax + 170];
    int mLen = 0;
};

bool IsDrawOp(uint8_t op) {
    return op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD || op == OP_LINE3D || op == OP_TEXRECT ||
           op == OP_TEXRECTFLIP || op == OP_FILLRECT;
}

} // namespace

struct Interp::NcState {
    int traceLeft, traceMode, traceSkip; /* menus-s2 [debug] nicensus_trace (NcTraceSet) */
    NcBucket* buckets[kNcBuckets];
    NcBucket* cur;       /* the open frame's bucket (null between frames) */
    int curBucket;
    int ctx;             /* current context */
    uint64_t lastTick;
    uint64_t frameT0;
    NcStatSnap last;
    NcAcc fAcc[NC_COUNT];
    uint32_t fOps[NC_COUNT][256];
    uint32_t fNrrEnters;
    uint32_t totalFrames;
    uint32_t sinceDump;
    uint32_t dumps;
    bool ncDumps;     /* [nicensus] receipts (false: the engine runs for [debug] ni_strict only) */
    int tag;          /* the current context's tag (NcTag) */
    /* [debug] ni_strict */
    int nsLevel;
    uint32_t nsEvery, nsSinceDump, nsDumps;
    int nsPrevBucket;
    NsTable* ns[kNcBuckets];
    NsTable* nsCur;
    uint32_t fStrict, fDisp, fDirect;
};

void Interp::NcSet(int on) {
    if (on == 0) {
        if (mNc != nullptr) {
            for (int b = 0; b < kNcBuckets; b++) {
                delete mNc->buckets[b];
                delete mNc->ns[b];
            }
            delete mNc;
            mNc = nullptr;
        }
        return;
    }
    if (mNc == nullptr) {
        mNc = new (std::nothrow) NcState;
        if (mNc != nullptr) {
            memset(static_cast<void*>(mNc), 0, sizeof(NcState));
            mNc->curBucket = -1;
            mNc->nsPrevBucket = -1;
        }
    }
    if (mNc != nullptr) {
        mNc->ncDumps = on == 1;
    }
}

void Interp::NsSet(int level, uint32_t every) {
    if (mNc == nullptr) {
        return;
    }
    mNc->nsLevel = level > 0 ? level : 0;
    mNc->nsEvery = every;
}

int Interp::NcTag(int tag) {
    NcState& s = *mNc;
    const int prev = s.tag;
    s.tag = tag & 0xFF;
    return prev;
}

/* The bucket's cell for key, created on first sight (null: the table is full, counted). */
static NsEntry* NsFind(NsTable& t, uint32_t key) {
    uint32_t h = key * 2654435761u;
    for (int probe = 0; probe < kNsSlots; probe++) {
        NsEntry& e = t.e[(h + static_cast<uint32_t>(probe)) & (kNsSlots - 1)];
        if (e.used == 0) {
            e.used = 1;
            e.key = key;
            e.firstF = t.frames;
            t.used++;
            return &e;
        }
        if (e.key == key) {
            return &e;
        }
    }
    t.overflow++;
    return nullptr;
}

/* One command into the open frame's strict table. Classes: disp = the walker's own dispatch words
 * (G_NOOP markers, G_DL, G_ENDDL) -- what the S7 marker dispatcher keeps; direct = a capture run's
 * rectangle / load the direct emitter drew; strict = everything else, a stock handler that ran. */
static void NsNote(Interp::NcState& s, uint8_t op, int site) {
    NsTable* t = s.nsCur;
    if (t == nullptr) {
        return;
    }
    if (site == Interp::NCS_HUDDIRECT) {
        s.fDirect++;
    } else if (s.ctx == Interp::NC_WALK && site == Interp::NCS_WALK && (op == OP_NOOP || op == OP_DL || op == OP_ENDDL)) {
        s.fDisp++;
    } else {
        s.fStrict++;
    }
    const uint32_t key = (static_cast<uint32_t>(s.ctx & 7) << 24) | (static_cast<uint32_t>(site & 15) << 16) |
                         (static_cast<uint32_t>(s.tag & 0xFF) << 8) | op;
    NsEntry* e = NsFind(*t, key);
    if (e == nullptr) {
        return;
    }
    e->n++;
    if (e->seq != s.totalFrames + 1u) {
        if (e->curF > e->maxF) {
            e->maxF = e->curF;
        }
        e->curF = 0;
        e->seq = s.totalFrames + 1u; /* +1: 0 is a fresh cell's "never" */
        e->frames++;
        if (e->lastVisit != t->visits) {
            e->lastVisit = t->visits;
            e->visits++;
        }
    }
    e->curF++;
    e->lastF = t->frames;
}

static NcStatSnap NcTakeSnap(const Counters& s) {
    NcStatSnap n;
    n.tris = s.tris;
    n.emit = s.trisEmitted;
    n.rects = s.rects;
    n.loads = s.tmemLoads;
    n.imports = s.texImports;
    n.draws = s.batches;
    n.vtx = s.vtx;
    return n;
}

static inline uint32_t NcDelta(uint32_t now, uint32_t before) {
    return now >= before ? now - before : now; /* a window reset in between: count from zero */
}

/* Closes the current context's interval: its ticks and counter deltas go to fAcc[ctx]. */
static void NcClose(Interp::NcState& s, const Counters& st) {
    const uint64_t now = static_cast<uint64_t>(gdx_ninterp_now());
    const NcStatSnap snap = NcTakeSnap(st);
    NcAcc& a = s.fAcc[s.ctx];
    a.ticks += now - s.lastTick;
    a.tris += NcDelta(snap.tris, s.last.tris);
    a.emit += NcDelta(snap.emit, s.last.emit);
    a.rects += NcDelta(snap.rects, s.last.rects);
    a.loads += NcDelta(snap.loads, s.last.loads);
    a.imports += NcDelta(snap.imports, s.last.imports);
    a.draws += NcDelta(snap.draws, s.last.draws);
    a.vtx += NcDelta(snap.vtx, s.last.vtx);
    s.last = snap;
    s.lastTick = now;
}

int Interp::NcSwitch(int ctx) {
    NcState& s = *mNc;
    const int prev = s.ctx;
    if (s.cur == nullptr || ctx < 0 || ctx >= NC_COUNT) {
        return prev;
    }
    NcClose(s, mStats);
    s.ctx = ctx;
    s.fAcc[ctx].enters++;
    if (ctx == NC_NRR) {
        s.fNrrEnters++;
    }
    return prev;
}

void Interp::NcFrameBegin(int bucket) {
    NcState& s = *mNc;
    if (bucket < 0 || bucket >= kNcBuckets) {
        bucket = (kNcModes - 1) * kNcSubs;
    }
    if (s.buckets[bucket] == nullptr) {
        s.buckets[bucket] = new (std::nothrow) NcBucket;
        if (s.buckets[bucket] == nullptr) {
            s.cur = nullptr;
            return;
        }
        memset(static_cast<void*>(s.buckets[bucket]), 0, sizeof(NcBucket));
    }
    s.cur = s.buckets[bucket];
    s.curBucket = bucket;
    s.ctx = NC_FRAME; /* RunFrame switches to NC_WALK around Walk() */
    s.tag = 0;
    if (s.nsLevel != 0) {
        if (s.ns[bucket] == nullptr) {
            s.ns[bucket] = new (std::nothrow) NsTable;
            if (s.ns[bucket] != nullptr) {
                memset(static_cast<void*>(s.ns[bucket]), 0, sizeof(NsTable));
            }
        }
        s.nsCur = s.ns[bucket];
        if (s.nsCur != nullptr && bucket != s.nsPrevBucket) {
            s.nsCur->visits++;
        }
        s.nsPrevBucket = bucket;
        s.fStrict = 0;
        s.fDisp = 0;
        s.fDirect = 0;
    }
    memset(s.fAcc, 0, sizeof(s.fAcc));
    memset(s.fOps, 0, sizeof(s.fOps));
    s.fNrrEnters = 0;
    s.last = NcTakeSnap(mStats);
    s.lastTick = static_cast<uint64_t>(gdx_ninterp_now());
    s.frameT0 = s.lastTick;
}

void Interp::NcFrameWipe(int type) {
    NcState& s = *mNc;
    if (s.cur != nullptr && type > 0) {
        s.cur->wipeType[type & 15]++;
    }
}

static void NcAddAcc(NcAcc& d, const NcAcc& a) {
    d.ticks += a.ticks;
    d.cmds += a.cmds;
    d.words += a.words;
    d.wordsRetained += a.wordsRetained;
    d.tris += a.tris;
    d.emit += a.emit;
    d.rects += a.rects;
    d.loads += a.loads;
    d.imports += a.imports;
    d.draws += a.draws;
    d.vtx += a.vtx;
    d.enters += a.enters;
}

void Interp::NcFrameEnd(const uint32_t* nrrMarkers, const uint32_t* nrrDraws, const uint32_t* nrrVerts, int nCats) {
    NcState& s = *mNc;
    if (s.cur == nullptr) {
        return;
    }
    NcClose(s, mStats);
    NcBucket& b = *s.cur;
    b.frames++;
    b.taskTicks += s.lastTick - s.frameT0;
    const bool nrrFrame = s.fNrrEnters != 0;
    if (nrrFrame) {
        b.nrrFrames++;
    }
    for (int c = 0; c < NC_COUNT; c++) {
        const int slot = (c == NC_WALK) ? (nrrFrame ? 0 : 1) : c + 1;
        NcAddAcc(b.acc[slot], s.fAcc[c]);
        for (int op = 0; op < 256; op++) {
            b.ops[slot][op] += s.fOps[c][op];
        }
    }
    for (int k = 0; k < nCats && k < kNcNrrCats; k++) {
        b.nrrMk[k] += nrrMarkers[k];
        b.nrrDr[k] += nrrDraws[k];
        b.nrrVx[k] += nrrVerts[k];
    }
    b.variant[mVariant & 7]++;
    if (s.nsCur != nullptr) {
        NsTable& t = *s.nsCur;
        t.frames++;
        t.strict += s.fStrict;
        t.disp += s.fDisp;
        t.direct += s.fDirect;
        if (s.fStrict > t.maxStrict) {
            t.maxStrict = s.fStrict;
        }
        if (s.fStrict == 0) {
            t.zeroStrict++;
        }
        t.hist[s.fStrict == 0 ? 0 : s.fStrict < 10 ? 1 : s.fStrict < 100 ? 2 : s.fStrict < 1000 ? 3 : 4]++;
        s.nsCur = nullptr;
    }
    s.cur = nullptr;
    s.totalFrames++;
    s.sinceDump++;
    if (s.nsLevel != 0) {
        s.nsSinceDump++;
        if (s.nsEvery != 0 && s.nsSinceDump >= s.nsEvery) {
            NsDump("sum", false);
        }
    }
}

void Interp::NcLusTask(int bucket) {
    NcState& s = *mNc;
    if (bucket < 0 || bucket >= kNcBuckets) {
        bucket = (kNcModes - 1) * kNcSubs;
    }
    if (s.buckets[bucket] == nullptr) {
        s.buckets[bucket] = new (std::nothrow) NcBucket;
        if (s.buckets[bucket] == nullptr) {
            return;
        }
        memset(static_cast<void*>(s.buckets[bucket]), 0, sizeof(NcBucket));
    }
    s.buckets[bucket]->lus++;
}

void Interp::NcNoteWords(uint32_t words, int retained) {
    NcState& s = *mNc;
    if (s.cur == nullptr) {
        return;
    }
    if (retained != 0) {
        s.fAcc[s.ctx].wordsRetained += words;
    } else {
        s.fAcc[s.ctx].words += words;
    }
}

static void NcNoteCombine(NcBucket& b, uint64_t comb) {
    uint32_t h = static_cast<uint32_t>(comb ^ (comb >> 29) ^ (comb >> 47)) * 2654435761u;
    for (int probe = 0; probe < kCombSet; probe++) {
        const int i = static_cast<int>((h + static_cast<uint32_t>(probe)) & (kCombSet - 1));
        if (b.combUsed[i] == 0) {
            b.combUsed[i] = 1;
            b.comb[i] = comb;
            b.combDistinct++;
            return;
        }
        if (b.comb[i] == comb) {
            return;
        }
    }
    b.combOverflow++;
}

void Interp::NcTraceSet(int lines, int mode, int skip) {
    if (mNc != nullptr) {
        mNc->traceLeft = lines > 0 ? lines : 0;
        mNc->traceMode = mode;
        mNc->traceSkip = skip > 0 ? skip : 0;
    }
}

void Interp::NcNote(const Cmd& c, bool f3d, int site) {
    NcState& s = *mNc;
    if (s.cur == nullptr) {
        return; /* outside a task (cannot happen on the owned path; kept safe) */
    }
    NcBucket& b = *s.cur;
    const uint8_t op = Opcode(c.w0);
    if (s.nsLevel != 0) {
        NsNote(s, op, site); /* [debug] ni_strict: counted, never dropped (level 1 = count) */
        if (op == OP_TEXRECT || op == OP_TEXRECTFLIP) {
            NsNote(s, OP_RDPHALF_1, site); /* the halves Exec consumes inline, as nicensus counts them */
            NsNote(s, OP_RDPHALF_2, site);
        }
    }
    if (__builtin_expect(s.traceLeft > 0, 0) && s.ctx == NC_WALK &&
        (s.traceMode < 0 || s.curBucket / kNcSubs == s.traceMode)) {
        if (s.traceSkip > 0) {
            s.traceSkip--;
        } else {
            s.traceLeft--;
            const Frame& f = mStack[mSp > 0 ? mSp - 1 : 0];
            GDXN_LOG("[nctrace] b=%02X.%d sp=%d list=%p idx=%u op=%02X %08X %08X", static_cast<unsigned>(s.curBucket / kNcSubs),
                     s.curBucket % kNcSubs, mSp, static_cast<const void*>(f.list.base), static_cast<unsigned>(f.idx), op,
                     c.w0, c.w1);
        }
    }
    s.fOps[s.ctx][op]++;
    s.fAcc[s.ctx].cmds++;
    if (op == OP_TEXRECT || op == OP_TEXRECTFLIP) {
        s.fOps[s.ctx][OP_RDPHALF_1]++; /* the two halves Exec consumes inline (as mStats counts them) */
        s.fOps[s.ctx][OP_RDPHALF_2]++;
        s.fAcc[s.ctx].cmds += 2;
    }
    if (f3d) {
        b.f3dCmds++;
    }
    switch (op) {
        case OP_MOVEWORD:
            b.moveword[(C0(c.w0, 16, 8) >> 1) & 15u]++;
            break;
        case OP_MOVEMEM:
            b.movemem[C0(c.w0, 0, 8) & 15u]++;
            break;
        case OP_SETTILE:
            b.tileFmt[(C0(c.w0, 21, 3) << 2) | C0(c.w0, 19, 2)]++;
            break;
        case OP_SETTIMG:
            b.timgFmt[(C0(c.w0, 21, 3) << 2) | C0(c.w0, 19, 2)]++;
            break;
        default:
            break;
    }
    if (!IsDrawOp(op)) {
        return;
    }
    /* The registers the draw will be resolved under (the state commands before it already ran). */
    const uint32_t h = mRdp.other_mode_h;
    const uint32_t l = mRdp.other_mode_l;
    const uint32_t g = mRsp.geometry_mode;
    b.drawCmds++;
    b.cycle[(h >> 20) & 3u]++;
    b.filt[(h >> 12) & 3u]++;
    b.tlut[(h >> 14) & 3u]++;
    b.zcu[((l >> 4) & 1u) | (((l >> 5) & 1u) << 1)]++;
    b.zmode[(l >> 10) & 3u]++;
    b.acmp[l & 3u]++;
    if (op != OP_TEXRECT && op != OP_TEXRECTFLIP && op != OP_FILLRECT) {
        b.geoLight += (g & 0x00020000u) ? 1u : 0u;
        b.geoFog += (g & 0x00010000u) ? 1u : 0u;
        b.geoTexGen += (g & 0x00040000u) ? 1u : 0u;
        b.geoCullF += (g & 0x00000200u) ? 1u : 0u;
        b.geoCullB += (g & 0x00000400u) ? 1u : 0u;
        b.geoZbuf += (g & 0x00000001u) ? 1u : 0u;
        b.geoShade += (g & 0x00000004u) ? 1u : 0u;
        b.geoSmooth += (g & 0x00200000u) ? 1u : 0u;
    }
    NcNoteCombine(b, mRdp.combine_mode);
}

/* Walk() with NcNote per command: the shipping loop's body, verbatim, plus the note. */
void Interp::WalkNicensus() {
    while (mSp > 0) {
        Frame& f = mStack[mSp - 1];
        if (f.idx >= f.list.limit) {
            mStats.runaway++;
            mSp--;
            continue;
        }
        Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        mStats.op[Opcode(c.w0)]++;
        mStats.cmds++;
        size_t f3dXfer = 0;
        if (f.list.isF3d && !RemapF3d(c, f.list, &f3dXfer)) {
            mStats.droppedOps++;
            continue;
        }
        if (mVerifyActive) {
            mBackend->VerifyNoteCmd(c.w0, c.w1);
        }
        NcNote(c, f.list.isF3d);
        const int r = Exec(c, f, f3dXfer);
        if (r == 1) {
            mSp--;
        }
    }
}

/* ------------------------------------------------------------------------- receipts ---- */

static void NcTopOps(const NcBucket& b, int slot, const char* head, double inv) {
    uint32_t t[256];
    memcpy(t, b.ops[slot], sizeof(t));
    uint64_t total = 0;
    for (int op = 0; op < 256; op++) {
        total += t[op];
    }
    if (total == 0) {
        return;
    }
    NcLine line(head);
    line.Add(" ops %s n=%.1f", kSlotName[slot], static_cast<double>(total) * inv);
    for (int rank = 0; rank < 16; rank++) {
        int best = -1;
        uint32_t bestN = 0;
        for (int op = 0; op < 256; op++) {
            if (t[op] > bestN) {
                bestN = t[op];
                best = op;
            }
        }
        if (best < 0) {
            break;
        }
        line.Add(" %02X=%.1f", static_cast<unsigned>(best), static_cast<double>(bestN) * inv);
        t[best] = 0;
    }
}

static void NcDumpBucket(const NcBucket& b, int bucket, const char* why, double msPerTick) {
    const int mode = bucket / Interp::kNcSubs;
    const int sub = bucket % Interp::kNcSubs;
    char head[96];
    snprintf(head, sizeof(head), "[nicensus] %s b=%02X.%d", why, static_cast<unsigned>(mode), sub);
    const double inv = b.frames != 0 ? 1.0 / static_cast<double>(b.frames) : 0.0;
    {
        uint64_t all = 0;
        for (int k = 0; k < kNcSlots; k++) {
            all += b.acc[k].cmds;
        }
        NcLine line(head);
        line.Add(" %s/%s frames=%u nrrframes=%u lus=%u cmds/f=%.1f", kModeName[mode], kSubName[sub], b.frames,
                 b.nrrFrames, b.lus, static_cast<double>(all) * inv);
        for (int k = 0; k < kNcSlots; k++) {
            line.Add(" %s=%.1f", kSlotName[k], static_cast<double>(b.acc[k].cmds) * inv);
        }
        line.Add(" | words nrrhud=%.1f hudret=%.1f | f3d=%.1f", static_cast<double>(b.acc[3].words) * inv,
                 static_cast<double>(b.acc[4].wordsRetained) * inv, static_cast<double>(b.f3dCmds) * inv);
        for (int t = 1; t < 16; t++) {
            if (b.wipeType[t] != 0) {
                line.Add(" wipe%d=%u", t, b.wipeType[t]); /* menus-s4: frames per transition type */
            }
        }
    }
    {
        /* nointerp N1 tripwire (docs/research/native/nointerp-plan.md §0, §2.5): walkB + walkW is
         * every command this frame that neither a marker/hook dispatch nor a compiled program
         * accounted for -- the walker executing a stock handler outside any owned region. Exit
         * criterion for "interpreter-free steady state" is this reading 0 on the steady sub-scope
         * of every screen; nonzero elsewhere (intro/post/change/first-sight record passes into
         * nrrhud/hud/seam/mesh) is expected and not what this line answers. */
        const uint64_t residue = b.acc[0].cmds + b.acc[1].cmds;
        NcLine line(head);
        line.Add(" [nointerp] residue=%.1f/f (walkB+walkW)", static_cast<double>(residue) * inv);
    }
    {
        NcLine line(head);
        line.Add(" ms task=%.3f", static_cast<double>(b.taskTicks) * msPerTick * inv);
        for (int k = 0; k < kNcSlots; k++) {
            line.Add(" %s=%.3f", kSlotName[k], static_cast<double>(b.acc[k].ticks) * msPerTick * inv);
        }
        line.Add(" | enters/f");
        for (int k = 2; k < kNcSlots; k++) {
            line.Add(" %s=%.1f", kSlotName[k], static_cast<double>(b.acc[k].enters) * inv);
        }
    }
    {
        NcLine line(head);
        line.Add(" geo tri/emit/rect/ld/imp/draw/vtx");
        for (int k = 0; k < kNcSlots; k++) {
            const NcAcc& a = b.acc[k];
            if (a.tris + a.rects + a.loads + a.draws + a.vtx + a.imports == 0) {
                continue;
            }
            line.Add(" %s=%.1f/%.1f/%.1f/%.1f/%.2f/%.1f/%.1f", kSlotName[k], a.tris * inv, a.emit * inv,
                     a.rects * inv, a.loads * inv, a.imports * inv, a.draws * inv, a.vtx * inv);
        }
    }
    {
        NcLine line(head);
        line.Add(" nrr mk/dr/vx");
        for (int k = 0; k < kNcNrrCats; k++) {
            if (b.nrrMk[k] + b.nrrDr[k] == 0) {
                continue;
            }
            line.Add(" %s=%.1f/%.1f/%.0f", kNrrCatName[k], b.nrrMk[k] * inv, b.nrrDr[k] * inv, b.nrrVx[k] * inv);
        }
    }
    for (int k = 0; k < kNcSlots; k++) {
        NcTopOps(b, k, head, inv);
    }
    {
        /* Every opcode the bucket executed, all contexts, as raw totals (rare ones included). */
        NcLine line(head);
        line.Add(" allops");
        for (int op = 0; op < 256; op++) {
            uint64_t n = 0;
            for (int k = 0; k < kNcSlots; k++) {
                n += b.ops[k][op];
            }
            if (n != 0) {
                line.Add(" %02X=%llu", static_cast<unsigned>(op), static_cast<unsigned long long>(n));
            }
        }
    }
    {
        const double dn = b.drawCmds != 0 ? 1.0 / static_cast<double>(b.drawCmds) : 0.0;
        NcLine line(head);
        line.Add(" feat drawcmds/f=%.1f cyc1/2/copy/fill=%.2f/%.2f/%.2f/%.2f", b.drawCmds * inv, b.cycle[0] * dn,
                 b.cycle[1] * dn, b.cycle[2] * dn, b.cycle[3] * dn);
        line.Add(" filt pt/bl/avg=%.2f/%.2f/%.2f", b.filt[0] * dn, b.filt[2] * dn, b.filt[3] * dn);
        line.Add(" tlut none/rgba/ia=%.2f/%.2f/%.2f", b.tlut[0] * dn, b.tlut[2] * dn, b.tlut[3] * dn);
        line.Add(" zcmp/upd -/c/u/cu=%.2f/%.2f/%.2f/%.2f", b.zcu[0] * dn, b.zcu[1] * dn, b.zcu[2] * dn,
                 b.zcu[3] * dn);
        line.Add(" zmode opa/int/xlu/dec=%.2f/%.2f/%.2f/%.2f", b.zmode[0] * dn, b.zmode[1] * dn, b.zmode[2] * dn,
                 b.zmode[3] * dn);
        line.Add(" acmp none/thr/dith=%.2f/%.2f/%.2f", b.acmp[0] * dn, b.acmp[1] * dn, b.acmp[3] * dn);
        line.Add(" geo(tri) light=%.1f fog=%.1f tgen=%.1f cullF=%.1f cullB=%.1f zbuf=%.1f shade=%.1f smooth=%.1f /f",
                 b.geoLight * inv, b.geoFog * inv, b.geoTexGen * inv, b.geoCullF * inv, b.geoCullB * inv,
                 b.geoZbuf * inv, b.geoShade * inv, b.geoSmooth * inv);
        line.Add(" combines=%u%s", b.combDistinct, b.combOverflow != 0 ? "+" : "");
    }
    {
        static const char* const kMw[8] = { "mtx", "numlight", "clip", "segment", "fog", "lightcol", "forcemtx",
                                             "perspnorm" };
        NcLine line(head);
        line.Add(" mw/f");
        for (int i = 0; i < 16; i++) {
            if (b.moveword[i] != 0) {
                line.Add(" %s=%.2f", i < 8 ? kMw[i] : "x", b.moveword[i] * inv);
            }
        }
        line.Add(" | mm/f");
        for (int i = 0; i < 16; i++) {
            if (b.movemem[i] != 0) {
                line.Add(" %d=%.2f", i, b.movemem[i] * inv);
            }
        }
        line.Add(" | variant");
        for (int i = 0; i < 8; i++) {
            if (b.variant[i] != 0) {
                line.Add(" v%d=%u", i, b.variant[i]);
            }
        }
    }
    {
        static const char* const kFmt[8] = { "rgba", "yuv", "ci", "ia", "i", "f5", "f6", "f7" };
        static const char* const kSiz[4] = { "4", "8", "16", "32" };
        NcLine line(head);
        line.Add(" settile/f");
        for (int i = 0; i < 32; i++) {
            if (b.tileFmt[i] != 0) {
                line.Add(" %s%s=%.2f", kFmt[i >> 2], kSiz[i & 3], b.tileFmt[i] * inv);
            }
        }
        line.Add(" | settimg/f");
        for (int i = 0; i < 32; i++) {
            if (b.timgFmt[i] != 0) {
                line.Add(" %s%s=%.2f", kFmt[i >> 2], kSiz[i & 3], b.timgFmt[i] * inv);
            }
        }
    }
}

void Interp::NcDump(const char* why, double msPerTick) {
    if (mNc == nullptr || !mNc->ncDumps) {
        return;
    }
    NcState& s = *mNc;
    s.dumps++;
    gdxn_logf("[nicensus] %s begin dump=%u frames=%u", why, s.dumps, s.totalFrames);
    for (int b = 0; b < kNcBuckets; b++) {
        const NcBucket* p = s.buckets[b];
        if (p == nullptr || (p->frames == 0 && p->lus == 0)) {
            continue;
        }
        NcDumpBucket(*p, b, why, msPerTick);
    }
    gdxn_logf("[nicensus] %s end dump=%u", why, s.dumps);
    s.sinceDump = 0;
}

void Interp::NcDumpEvery(uint32_t every, double msPerTick) {
    if (mNc == nullptr || !mNc->ncDumps || every == 0 || mNc->sinceDump < every) {
        return;
    }
    NcDump("sum", msPerTick);
}

/* ------------------------------------------------------------------ [debug] ni_strict ---- */

static void NsDumpBucket(const NsTable& t, int bucket, const char* why, bool full) {
    const int mode = bucket / Interp::kNcSubs;
    const int sub = bucket % Interp::kNcSubs;
    const double inv = t.frames != 0 ? 1.0 / static_cast<double>(t.frames) : 0.0;
    char head[64];
    snprintf(head, sizeof(head), "[nistrict] %s b=%02X.%d", why, static_cast<unsigned>(mode), sub);
    {
        NcLine line(head);
        line.Add(" %s/%s frames=%u visits=%u strict/f=%.2f disp/f=%.2f direct/f=%.2f maxf=%u zero=%u", kModeName[mode],
                 kSubName[sub], t.frames, t.visits, static_cast<double>(t.strict) * inv,
                 static_cast<double>(t.disp) * inv, static_cast<double>(t.direct) * inv, t.maxStrict, t.zeroStrict);
        line.Add(" hist=%u/%u/%u/%u/%u cells=%u ovf=%u", t.hist[0], t.hist[1], t.hist[2], t.hist[3], t.hist[4], t.used,
                 t.overflow);
    }
    /* The cells by command count, largest first (selection: tables are small and this is cold). */
    static uint8_t taken[kNsSlots];
    memset(taken, 0, sizeof(taken));
    const int limit = full ? 160 : 6;
    NcLine* compact = full ? nullptr : new (std::nothrow) NcLine(head);
    if (compact != nullptr) {
        compact->Add(" top");
    }
    for (int rank = 0; rank < limit; rank++) {
        int best = -1;
        uint32_t bestN = 0;
        for (int i = 0; i < kNsSlots; i++) {
            if (t.e[i].used != 0 && taken[i] == 0 && t.e[i].n > bestN) {
                bestN = t.e[i].n;
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        taken[best] = 1;
        const NsEntry& e = t.e[best];
        const int ctx = static_cast<int>((e.key >> 24) & 7u);
        const int site = static_cast<int>((e.key >> 16) & 15u);
        const unsigned tag = (e.key >> 8) & 0xFFu;
        const unsigned op = e.key & 0xFFu;
        const uint32_t maxF = e.curF > e.maxF ? e.curF : e.maxF;
        if (compact != nullptr) {
            compact->Add(" %s/%s/%02X/%02X=%.2f", ctx < Interp::NC_COUNT ? kNsCtxName[ctx] : "?",
                         site < Interp::NCS_COUNT ? kNsSiteName[site] : "?", tag, op, static_cast<double>(e.n) * inv);
            continue;
        }
        gdxn_logf("%s e c=%s t=%02X s=%s op=%02X n=%u fr=%u vis=%u max=%u first=%u last=%u", head,
                  ctx < Interp::NC_COUNT ? kNsCtxName[ctx] : "?", tag, site < Interp::NCS_COUNT ? kNsSiteName[site] : "?",
                  op, e.n, e.frames, e.visits, maxF, e.firstF, e.lastF);
    }
    delete compact;
}

void Interp::NsDump(const char* why, bool full) {
    if (mNc == nullptr || mNc->nsLevel == 0) {
        return;
    }
    NcState& s = *mNc;
    s.nsDumps++;
    gdxn_logf("[nistrict] %s begin dump=%u frames=%u level=%d", why, s.nsDumps, s.totalFrames, s.nsLevel);
    for (int b = 0; b < kNcBuckets; b++) {
        const NsTable* t = s.ns[b];
        if (t == nullptr || t->frames == 0) {
            continue;
        }
        NsDumpBucket(*t, b, why, full);
    }
    gdxn_logf("[nistrict] %s end dump=%u", why, s.nsDumps);
    s.nsSinceDump = 0;
}

} // namespace gdxn
